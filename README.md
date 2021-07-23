# FUSE Reference FS (ReFS)

In this (2-part) assignment,
you will implement a variant of the reference file system described in the textbook
([chapter 40](http://pages.cs.wisc.edu/~remzi/OSTEP/file-implementation.pdf)).
You may create additional files if you would like,
but your FUSE file system's `main()` method should be in a file named `refs.c`.

 * [Lab Handout](http://cs.williams.edu/~jannen/teaching/s21/cs333/labs/fuse/fuse-fs.html)


## Repository Contents

 * __`bitmap.c`__: a straightforward implementation of a bitmap data structure that supports
   setting/clearing individual bits, as well as querying a given bit's value.
 * __`refs.h`__: struct and function declarations for the implementation of ReFS.
 * __`refs.c`__: definitions of the main functions that define ReFS behavior.
 * __`writetests.c`__: c file for testing writes to different offsets
 * __`Makefile`__: includes rules to compile ReFS as well as a testable version of the bitmap code (`make bitmap`)
 * __`big.c`__: meaningnless c file used to test writing big text files
 * __`12.c`__: meaningnless c file used to test writing big text files 
 * __`debugsetup.sh`__: shell script for unmounting, making and running gdb on refs
 * __`setup.sh`__: shell script for unmounting, making and running refs -s -d mnt
 * __`lotsofdirs.sh`__: shell script for testing creating lots of directories and writing to files inside those directories
 * __`clearlotsofdirs.sh`__: shell script for clearing the files/directories made by lotsofdirs.s 
 * __`lotsofsubdirs.sh`__: shell script for testing creating lots of directories and subdirectories and writing to files inside those directories
 * __`clearlotsofsubdirs.sh`__: shell script for clearing the files/directories made by clearlotsofdirs.sh
 * __`lstest.sh`__: shell script for testing the output of ls against a regular directory with identical contents. NOTE: assumes mnt directory is empty
 * __`writetests.sh`__: shell script for writes at various offsets
 * __`ddtest.sh`__: tests writes of a big file for various write sizes
 * __`dog.jpg`__: image file for testing writing big files. after ddtest, you should be able to view the images in mnt. On some systmes, this can be accomplished with `xdg-open mnt/dog.jpg`
## Implementation details

Compile refs with `make`

Comple `writetest.c` with `gcc -o writetest writetest.c`

Note that `writetest.sh` assumes writetest.c has been compiled to an executable called `writetest`. 

All .sh test files assume that the filesystem is mounted on a directory titled `mnt`. 
