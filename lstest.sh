#!/bin/bash
rm -rf mnt2
mkdir mnt2
for (( n=0; n<300; n++ ))
do
    mkdir "mnt/test$n"
    mkdir "mnt2/test$n"
    echo "hello" > "mnt/file$n"
    echo "hello" > "mnt2/file$n"
done
ls mnt > mntls
ls mnt2 > mnt2ls
echo "first diff:"
diff mnt2ls mntls
for (( n=0; n<300; n++ ))
do
    mkdir "mnt/test200/subdir$n"
    mkdir "mnt2/test200/subdir$n"
    echo "hello" > "mnt/test200/subfile$n"
    echo "hello" > "mnt2/test200/subfile$n"
done
ls mnt/test200 > mntls
ls mnt2/test200 > mnt2ls
echo "second diff:"
diff mnt2ls mntls

# echo "" > mnt/foo.txt
# echo "" > foo.txt
# ./writetest mnt/foo.txt 15 "newdata"
# ./writetest foo.txt 15 "newdata"
# echo "first diff"
# diff foo.txt mnt/foo.txt

# ./writetest mnt/foo.txt 6000 "into second block"
# ./writetest foo.txt 6000 "into second block"
# echo "second diff"
# diff foo.txt mnt/foo.txt


# ./writetest mnt/foo.txt  60000 "into indirect block"
# ./writetest foo.txt 60000 "into indirect block"
# echo "third diff"
# diff foo.txt mnt/foo.txt

# ./writetest mnt/foo.txt  500000 "deep into file"
# ./writetest foo.txt 500000 "deep into file"
# echo "fourth diff"
# diff foo.txt mnt/foo.txt

# rm foo.txt
# rm mnt/foo.txt


