#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_mutex.h>

/* GL_AMD_pinned_memory */
#ifndef GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD
#define GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD 0x9160
#endif

static void *(*libc_malloc)(size_t);
static void *(*libc_realloc)(void *, size_t);
static void *(*libc_calloc)(size_t, size_t);
static void (*libc_free)(void *);

static GLenum (*pglGetError)();
static void (*pglGenBuffers)(GLsizei, GLuint *);
static void (*pglDeleteBuffers)(GLsizei, const GLuint *);
static void (*pglBindBuffer)(GLenum, GLuint);
static void (*pglBufferStorage)(GLenum, GLsizeiptr, const GLvoid *, GLbitfield);
static GLvoid *(*pglMapBuffer)(GLenum, GLenum);
static GLboolean (*pglUnmapBuffer)(GLenum);
static void (*pglGetIntegerv)(GLenum, GLint *);
static const GLubyte *(*pglGetStringi)(GLenum, GLint);

struct node {
	void *address;
	size_t size;
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
	int backing;
} gContext;

/* 16MB of GLuint handles - 0.5MB bitset */
#define HANDLES 4000000
#define BITSET ((HANDLES)/8+(!!((HANDLES)%8)))

__attribute__((constructor))
static void moreram_ctor(void) {
	if (SDL_AtomicCAS(&gContext.instances, 0, 1) != 0) {
		/* Prevent initializing it more than once */
		return;
	}

	*(void **)&libc_malloc = dlsym(RTLD_NEXT, "malloc");
	*(void **)&libc_realloc = dlsym(RTLD_NEXT, "realloc");
	*(void **)&libc_calloc = dlsym(RTLD_NEXT, "calloc");
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
	*(void **)&pglGetIntegerv = SDL_GL_GetProcAddress("glGetIntegerv");
	*(void **)&pglGetStringi = SDL_GL_GetProcAddress("glGetStringi");

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

	/* Standard backing buffer type */
	gContext.backing = GL_ARRAY_BUFFER;

	/* Check if we haave GL_AMD_pinned_memory extension */
	GLint extensions = 0;
	pglGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
	for (GLint i = 0; i < extensions; i++) {
		const GLubyte *extension = pglGetStringi(GL_EXTENSIONS, i);
		if (strcmp((const char *)extensions, "AMD_pinned_memory"))
			continue;
		/* We support AMDs pinned memory extension - change backing type */
		gContext.backing = GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD;
		break;
	}

	SDL_AtomicIncRef(&gContext.instances);
}

__attribute__((destructor))
static void moreram_dtor(void) {
	if (SDL_AtomicGet(&gContext.instances) == 1) {
		/* Unmap any remaining buffers */
		SDL_LockMutex(gContext.lock);
		for (struct node *n = gContext.head; n; ) {
			pglBindBuffer(gContext.backing, gContext.handles[n->bit]);
			/* The linked list nodes themselves are represented by the
			 * memory obtained with glMapBuffer. We need to load the
			 * address of the next node before we unmap the buffer. The
			 * compiler is free to reorder the load here so we need a
			 * barrier here to ensure the load order. The barrier after
			 * the unmap ensures it does not reorder the write.
			 */
			struct node *next = n->next;
			SDL_CompilerBarrier();
			pglUnmapBuffer(gContext.backing);
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
	pglBindBuffer(gContext.backing, handle);
	pglBufferStorage(gContext.backing, bytes, NULL, GL_MAP_COHERENT_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);

	/* Get the memory from OpenGL */
	struct node *node = pglMapBuffer(gContext.backing, GL_READ_WRITE);
	if (!node) {
		/* Can't map the memory - definitely out of it! */
		errno = ENOMEM;
		SDL_UnlockMutex(gContext.lock);
		return NULL;
	}

	node->size = bytes - sizeof(struct node);
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
		pglBindBuffer(gContext.backing, gContext.handles[n->bit]);

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
		pglUnmapBuffer(gContext.backing);

		SDL_UnlockMutex(gContext.lock);
		return;
	}
	SDL_UnlockMutex(gContext.lock);
	/* Not part of the GL heap so forward to libc's free */
	libc_free(address);
}

void *realloc(void *address, size_t size) {
	/* Consistency with glibc realloc */
	if (size == 0) {
		free(address);
		return NULL;
	}

	/* Walk the entire GL heap to see if this pointer exists in there */
	SDL_LockMutex(gContext.lock);
	for (struct node *n = gContext.head; n; n = n->next) {
		if (n->address != address)
			continue;

		/* No need to resize in this case */
		if (n->size >= size) {
			n->size = size;
			SDL_UnlockMutex(gContext.lock);
			return address;
		}

		/* Requests some memory for the resize */
		SDL_UnlockMutex(gContext.lock);
		void *resize = malloc(size);
		if (!resize)
			return NULL;
		SDL_LockMutex(gContext.lock);

		/* Bind the current handle before unlinking it */
		pglBindBuffer(gContext.backing, gContext.handles[n->bit]);

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

		/* Copy the memory into the resize */
		memcpy(resize, address, n->size);

		/* The linked list structure is maintained by the memory obtained
		 * from GL. To prevent the compiler from reordering the read of
		 * n->bit above below this unmap call we use a compiler barrier
		 * here. */
		SDL_CompilerBarrier();

		/* Unmap the memory it references */
		pglUnmapBuffer(gContext.backing);

		SDL_UnlockMutex(gContext.lock);
		return resize;
	}
	SDL_UnlockMutex(gContext.lock);

	/* Not part of the GL heap so forward to libc's realloc */
	return libc_realloc(address, size);
}

void *calloc(size_t m, size_t n) {
	if (n && m > (size_t)-1/n) {
		errno = ENOMEM;
		return 0;
	}
	void *attempt = libc_calloc(m, n);
	return attempt ? attempt : memset(malloc(m*n), 0, m*n);
}
