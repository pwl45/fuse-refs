#!/bin/bash
echo "begin rm"
for (( n=0; n<700; n++ ))
do
if [ $((n%27)) -eq 0 ]
    then
    rm mnt/test65/subdir$n/copy.c
    fi
done
for (( n=0; n<700; n++ ))
do
    rmdir "mnt/test65/subdir$n"
done
for (( n=0; n<70; n++ ))
do
    rmdir "mnt/test$n"
done
# echo "begin rm"
# for (( n=0; n<700; n++ ))
# do
# 	if [ $((n%27)) -eq 0 ]
#     then
#     rm mnt/test$n/copy.c
#     fi
# done
# echo "begin rmdir"
# for (( n=0; n<700; n++ ))
# do
#     rmdir "mnt/test$n"
# done
