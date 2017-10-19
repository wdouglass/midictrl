CFLAGS=-Wall --std=c99

midictrl: midictrl.o
	gcc -o $@ $^
