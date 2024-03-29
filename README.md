# Linux-Dynamic-Memory-Manager

---


## 📒 Table of Contents
- [📍 Overview](#-overview)
- [📂 Project Structure](#-project-structure)
- [🎮 Using linux-dynamic-memory-manager](#-using-linux-dynamic-memory-manager)
- [🚀 Demo](#-demo)


---


## 📍 Overview
The project implements its own malloc() and free() calls to allocate and free memory dynamically. 

The memory manager uses a linked list of free blocks to track available memory. When xcalloc() is called, the memory manager searches the free list for a block that is large enough to satisfy the request. If a suitable block is found, the block is removed from the free list and returned to the caller. If no suitable block is found, a new block is allocated from the operating system. When xfree() is called, the memory manager adds the freed block to the free list.

The memory manager includes features to improve performance and memory utilization, such as block merging and fragmentation avoidance.


---


## 📂 Project Structure


```bash
repo
├── Makefile
├── README.md
└── src
    ├── glthreads
    │   ├── Makefile
    │   ├── inc
    │   │   └── glthreads.h
    │   └── src
    │       └── glthreads.c
    ├── mem_mang
    │   ├── Makefile
    │   ├── inc
    │   │   ├── mm.h
    │   │   └── uapi_mm.h
    │   └── src
    │       └── mm.c
    └── test_app
        ├── Makefile
        └── src
            └── test_app.c

```


---


## 🎮 Using linux-dynamic-memory-manager

```bash
git clone https://github.com/kaustubhshan27/linux-heap-memory-manager
cd linux-heap-memory-manager
make build_dir
make all
./bins/test_app
```


---


## 🚀 Demo


Testing memory allocation and deallocation:


![lhmm](https://github.com/kaustubhshan27/linux-heap-memory-manager/assets/32894621/1047f340-1cb4-4e9a-88f1-63579e203f39)
