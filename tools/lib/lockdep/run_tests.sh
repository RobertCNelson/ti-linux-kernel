#! /bin/bash

make &> /dev/null

for i in `ls tests/*.c`; do
	testname=$(basename -s .c "$i")
	gcc -o tests/$testname -lpthread $i liblockdep.a -Iinclude -D__USE_LIBLOCKDEP &> /dev/null
	echo -ne "$testname... "
	if [ $(timeout 1 ./tests/$testname | wc -l) -gt 0 ]; then
		echo "PASSED!"
	else
		echo "FAILED!"
	fi
	rm tests/$testname
done

for i in `ls tests/*.c`; do
	testname=$(basename -s .c "$i")
	gcc -o tests/$testname -lpthread -Iinclude $i &> /dev/null
	echo -ne "(PRELOAD) $testname... "
	if [ $(LD_PRELOAD=./liblockdep.so timeout 1 ./tests/$testname | wc -l) -gt 0 ]; then
		echo "PASSED!"
	else
		echo "FAILED!"
	fi
	rm tests/$testname
done
