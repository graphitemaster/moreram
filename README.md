# More Ram

More Ram is a system-wide malloc replacement which allows you to gain up
to an additional 4GB of system RAM.

To use:
```
$ LD_PRELOAD=moreram.so ./app
```

# How it works

More Ram exploits the video memory using OpenGL to unlock that memory so
you can open a couple more tabs in Firefox

# Technical description and difficulties

By taking advantage of persistently mapped buffers in OpenGL, we can exploit
up to an additional 4GB of memory for system tasks.

There is no guarantee that the persistently mapped buffer technique actually
references video memory. The worst case it's shadow memory and this actually
wastes memory.

This will work on embedded better than it will on Desktops, where persistently
mapped buffers actually do reference video memory. As for desktop, your best
best is a modern NV graphic card.

# TODO

* Try AMD_pinned_memory for AMD GPUs

# Dependencies
Unfortunately for this to work you need a working X server as a GL context
does need to be created

* SDL2
* GL 3.3+
* Working X server

# Is this a joke?

Totally a joke but still fun none the less. But it *can* work.

Why not downloadmoreram ;)

