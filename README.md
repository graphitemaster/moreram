# More Ram

More Ram is a system-wide malloc replacement which allows you to gain up
to an additional 12GB of system RAM.

To use:
```
$ sysctl -w vm.overcommit_memory=2
$ LD_PRELOAD=moreram.so ./app
```

# How it works

More Ram exploits the video memory using OpenGL to unlock that memory so
you can open a couple more tabs in Firefox

# Technical description and difficulties

By taking advantage of persistently mapped buffers in OpenGL, we can exploit
up to an additional 12GB of memory for system tasks.

There is no guarantee that the persistently mapped buffer technique actually
references video memory. The worst case it's shadow memory and this actually
wastes memory.

This will work on embedded better than it will on Desktops, where persistently
mapped buffers actually do reference video memory. As for desktop, your best
bet is a modern NV graphic card or an AMD GPU which supports GL_AMD_pinned_memory.

The logic for using GPU memory is only triggered when malloc/calloc/realloc
fail. On systems with overcommit, this will never be the case. To make
use of this overcommit *must be disabled*

Oh yeah, and it's thread safe!

# Dependencies
Unfortunately for this to work you need a working X server as a GL context
does need to be created

* SDL2
* GL 3.3+
* Working X server

# Is this a joke?

Totally a joke but still fun none the less. But it *can* work.

Why not downloadmoreram ;)

