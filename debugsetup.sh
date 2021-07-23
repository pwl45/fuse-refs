#!/bin/bash
fusermount -u mnt
make
gdb refs
