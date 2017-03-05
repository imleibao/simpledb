pager: os.o util.o
	gcc -o gager util.o os.o

clean:
	rm pager util.o os.o
