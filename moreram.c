#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_mutex.h>

static void *(*libc_malloc)(size_t);
static void (*libc_free)(void *);

static GLenum (*pglGetError)();
static void (*pglGenBuffers)(GLsizei, GLuint *);
static void (*pglDeleteBuffers)(GLsizei, const GLuint *);
static void (*pglBindBuffer)(GLenum, GLuint);
static void (*pglBufferStorage)(GLenum, GLsizeiptr, const GLvoid *, GLbitfield);
static GLvoid *(*pglMapBuffer)(GLenum, GLenum);
static GLboolean (*pglUnmapBuffer)(GLenum);

struct node {
	void *address;
	size_t bit;
	struct node *next;
	struct node *prev;
};

static struct {
	SDL_mutex *lock;
	SDL_atomic_t instances;
	SDL_GLContext context;
	GLuint *handles;
	unsigned int *bitset;
	struct node *head;
	struct node *tail;
} gContext;

/* 16MB of GLuint handles - 0.5MB bitset */
#define HANDLES 4000000
#define BITSET ((HANDLES)/8+(!!((HANDLES)%8)))

__attribute__((constructor))
static void moreram_ctor(void) {
	/* Prevent initializing if already in the address space */
	if (SDL_AtomicGet(&gContext.instances) != 0) {
		/* An additional instance */
		SDL_AtomicIncRef(&gContext.instances);
		return;
	}

	*(void **)&libc_malloc = dlsym(RTLD_NEXT, "malloc");
	*(void **)&libc_free = dlsym(RTLD_NEXT, "free");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		/* Something terrible has happened: if we fail this early there
		 * is absolutely nothing that can be done */
		abort();
	}

	if (!(gContext.lock = SDL_CreateMutex()))
		abort();

	/* Create the window such that it's definitely invisible */
	SDL_Window *window = SDL_CreateWindow("",
	                                      SDL_WINDOWPOS_UNDEFINED,
	                                      SDL_WINDOWPOS_UNDEFINED,
	                                      1,
	                                      1,
	                                      SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);

	assert(window);

	gContext.context = SDL_GL_CreateContext(window);
	/* No longer need the window */
	SDL_DestroyWindow(window);

	/* Get the addresses of the GL functions we'll be using */
	*(void **)&pglGetError = SDL_GL_GetProcAddress("glGetError");
	*(void **)&pglGenBuffers = SDL_GL_GetProcAddress("glGenBuffers");
	*(void **)&pglDeleteBuffers = SDL_GL_GetProcAddress("glDeleteBuffers");
	*(void **)&pglBindBuffer = SDL_GL_GetProcAddress("glBindBuffer");
	*(void **)&pglBufferStorage = SDL_GL_GetProcAddress("glBufferStorage");
	*(void **)&pglMapBuffer = SDL_GL_GetProcAddress("glMapBuffer");
	*(void **)&pglUnmapBuffer = SDL_GL_GetProcAddress("glUnmapBuffer");

	/* Ensure we have some handles available to do mappings with since
	 * glGenBuffers uses system-wide malloc which will be running out
	 * of memory. */
	if (!(gContext.handles = malloc(HANDLES * sizeof(GLuint))))
		abort();

	pglGenBuffers(BITSET, gContext.handles);
	if (pglGetError() == GL_OUT_OF_MEMORY)
		abort();

	/* Now allocate a bitset to keep track of which handles are mapped */
	if (!(gContext.bitset = libc_malloc(BITSET)))
		abort();

	/* Set all those bits to zero */
	memset(gContext.bitset, 0, BITSET);
}

__attribute__((destructor))
static void moreram_dtor(void) {
	if (SDL_AtomicGet(&gContext.instances) == 1) {
		/* Unmap any remaining buffers */
		SDL_LockMutex(gContext.lock);
		for (struct node *n = gContext.head; n; ) {
			pglBindBuffer(GL_ARRAY_BUFFER, gContext.handles[n->bit]);
			/* The linked list nodes themselves are represented by the
			 * memory obtained with glMapBuffer. We need to load the
			 * address of the next node before we unmap the buffer. The
			 * compiler is free to reorder the load here so we need a
			 * barrier here to ensure the load order. The barrier after
			 * the unmap ensures it does not reorder the write.
			 */
			struct node *next = n->next;
			SDL_CompilerBarrier();
			pglUnmapBuffer(GL_ARRAY_BUFFER);
			SDL_CompilerBarrier();
			n = next;
		}
		/* Release the handles */
		pglDeleteBuffers(HANDLES, gContext.handles);
		/* Release the bitset */
		libc_free(gContext.bitset);
		/* Destroy the context */
		SDL_GL_DeleteContext(gContext.context);
		SDL_UnlockMutex(gContext.lock);
		/* Destroy the mutex */
		SDL_DestroyMutex(gContext.lock);
		/* Shutdown SDL */
		SDL_Quit();
	}
	/* One less instance */
	SDL_AtomicDecRef(&gContext.instances);
}

void *malloc(size_t bytes) {
	void *attempt = libc_malloc(bytes);
	if (attempt)
		return attempt;

	/* Additional memory needed for our header */
	bytes += sizeof(struct node);

	SDL_LockMutex(gContext.lock);

	/* For every byte in the bit set */
	size_t i = 0;
	size_t j = 0;
	for (; i < HANDLES / 8; i++) {
		/* For every bit in byte */
		for (j = 0; j < 8; j++)
			if (!(gContext.bitset[i] & (1 << j)))
				break;
	}
	/* Out of handles ? */
	if (i == HANDLES / 8 && j == 8) {
		errno = ENOMEM;
		SDL_UnlockMutex(gContext.lock);
		return NULL;
	}

	size_t bit = i*8+j;
	GLuint handle = gContext.handles[bit];
	pglBindBuffer(GL_ARRAY_BUFFER, handle);
	pglBufferStorage(GL_ARRAY_BUFFER, bytes, NULL, GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT | GL_MAP_WRITE_BIT);

	/* Get the memory from OpenGL */
	struct node *node = pglMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
	if (!node) {
		/* Can't map the memory - definitely out of it! */
		errno = ENOMEM;
		SDL_UnlockMutex(gContext.lock);
		return NULL;
	}

	node->address = node + 1;
	node->bit = bit;

	/* Mark the handle as being used */
	gContext.bitset[bit / 8] |= (1 << (bit % 8));

	/* Maintain the linked list structure */
	if (gContext.tail) {
		gContext.tail->next = node;
		node->prev = gContext.tail;
		gContext.tail = node;
	} else {
		gContext.head = node;
		gContext.tail = node;
	}

	SDL_UnlockMutex(gContext.lock);
	return node + 1;
}

void free(void *address) {
	/* Walk the entire GL heap to see if this pointer exists in there */
	SDL_LockMutex(gContext.lock);
	for (struct node *n = gContext.head; n; n = n->next) {
		if (n->address != address)
			continue;

		/* Bind the current handle before unlinking it */
		pglBindBuffer(GL_ARRAY_BUFFER, gContext.handles[n->bit]);

		/* Unlink it from the linked list */
		if (n == gContext.head && n == gContext.tail) {
			gContext.head = NULL;
			gContext.tail = NULL;
		} else if (n == gContext.head) {
			gContext.head = n->next;
			gContext.head->prev = NULL;
		} else if (n == gContext.tail) {
			gContext.tail = n->prev;
			gContext.tail->next = NULL;
		} else {
			struct node *next = n->next;
			struct node *prev = n->prev;
			next->prev = prev;
			prev->next = next;
		}

		/* Mark the memory as being available again in the bitset */
		gContext.bitset[n->bit / 8] &= ~(1 << (n->bit % 8));

		/* The linked list structure is maintained by the memory obtained
		 * from GL. To prevent the compiler from reordering the read of
		 * n->bit above below this unmap call we use a compiler barrier
		 * here. */
		SDL_CompilerBarrier();

		/* Unmap the memory it references */
		pglUnmapBuffer(GL_ARRAY_BUFFER);

		SDL_UnlockMutex(gContext.lock);
		return;
	}
	SDL_UnlockMutex(gContext.lock);
	/* Not part of the GL heap so forward to libc's free */
	libc_free(address);
}
