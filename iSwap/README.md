# iSwap

Link of iSwap paper: [iSwap: A New Memory Page Swap Mechanism for Reducing Ineffective I/O](https://liulei-sys-inventor.github.io/files/iSwap.pdf)



iSwap is a reuse pattern and learning-based memory page swap mechanism, which reduces the ineffective I/O swap operations and improves the QoS for applications with a high priority in modern OSes. iSwap works well on the latest off-the-shelf Intel/ARM(Phytium CPUs) platforms.

## iSwap components

iSwap works in the Linux kernel 5.10. It consists of 4 main components:

1. **Monitoring the Page Presence (PPM)**: This component captures the page-swapping

   information. Locate in the  Linux kernel.

2. **Page-level logic Reuse pattern Sampling (PRS) **: This component learns whether a specific page will be used. Implement as a kernel module. Locates in the folder /kernel_module.

3. **Swap Thread**:  This component manages the pages based on the reuse patterns and conducts swaps that avoid moving hot and will-be-used pages out. Locate in the  Linux kernel.

4. **Compress and Swap Thread**: This component distinguishes applications’ pages and applies different swap strategies – compress LC applications’ pages (high priority) in the main memory and swap out BE applications’ pages (low priority) to hard disk. Locate in the  Linux kernel.

The four components in iSwap work in parallel as independent threads in the kernel.

## How to Run iSwap

1. Make and install the iSwap's Linux kernel:

```
cd linux_kernel
make -j64
make INSTALL_MOD_STRIP=1 modules_install
make install
```

2. Enable iSwap's Linux kernel and reboot.
3. Enable zswap:

```
echo 1 > /sys/module/zswap/parameters/enabled
```

4. Make  and insert iSwap's kernel module:

```
cd kernel_module
make
insmod iswap.ko
```

If you want to remove iSwap's kernel module:

```
rmmod iswap.ko
```

5. Ready to run users' applications. And use `nice/renice` to specify the high priority applications.