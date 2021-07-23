#!/bin/bash
for (( n=0; n<700; n++ ))
do
    mkdir "mnt/test$n"
done
for (( n=0; n<700; n++ ))
do
if [ $((n%27)) -eq 0 ]
    then
    cp 12.c mnt/test$n/copy.c
    fi
done
# Test that state is persistent after unmounting and remounting
echo "begin diff"
for (( n=0; n<700; n++ ))
do
	if [ $((n%27)) -eq 0 ]
    then
    diff 12.c mnt/test$n/copy.c
    fi
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
