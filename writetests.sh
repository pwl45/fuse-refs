#!/bin/bash
# This will output an error about not setting times with touch, but that's okay
touch foo.txt
touch mnt/foo.txt
echo "" > mnt/foo.txt
echo "" > foo.txt
./writetest mnt/foo.txt 15 "newdata"
./writetest foo.txt 15 "newdata"
echo "0th diff"
diff foo.txt mnt/foo.txt

./writetest mnt/foo.txt 18 "extending file outside"
./writetest foo.txt 18 "extending file outside"
echo "first diff"
diff foo.txt mnt/foo.txt

./writetest mnt/foo.txt 6000 "into second block"
./writetest foo.txt 6000 "into second block"
echo "second diff"
diff foo.txt mnt/foo.txt


./writetest mnt/foo.txt  60000 "into indirect block"
./writetest foo.txt 60000 "into indirect block"
echo "third diff"
diff foo.txt mnt/foo.txt

./writetest mnt/foo.txt  500000 "deep into file"
./writetest foo.txt 500000 "deep into file"
echo "fourth diff"
diff foo.txt mnt/foo.txt

./writetest mnt/foo.txt  8186 "cross-block write, middle of file"
./writetest foo.txt  8186 "cross-block write, middle of file"
echo "fifth diff"
diff foo.txt mnt/foo.txt

./writetest mnt/foo.txt  8180 "cross-block overwrite!"
./writetest foo.txt  8180 "cross-block overwrite!"
echo "sixth diff"
diff foo.txt mnt/foo.txt

# rm foo.txt
# rm mnt/foo.txt
