#!/bin/bash
(dd if=dog.jpg of=mnt/dog.jpg) > /dev/null
(dd if=dog.jpg of=mnt/dog2.jpg bs=4000) > /dev/null
(dd if=dog.jpg of=mnt/dog3.jpg bs=8000) > /dev/null
(dd if=dog.jpg of=mnt/dog4.jpg bs=941772) > /dev/null

echo "----------------------------------BEGIN DIFFS----------------------------------"
diff dog.jpg mnt/dog.jpg
diff dog.jpg mnt/dog2.jpg
diff dog.jpg mnt/dog3.jpg
diff dog.jpg mnt/dog4.jpg
echo "-----------------------------------END DIFFS-----------------------------------"


