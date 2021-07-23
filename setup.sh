#!/bin/bash
fusermount -u mnt
make
./refs -s -d mnt
