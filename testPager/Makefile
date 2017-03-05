COMPILE=gcc -g -c

pager: os.o util.o hash.o pager.o testPager.o random.o
	gcc -o testPager util.o os.o pager.o testPager.o hash.o \
	random.o

util.o: util.c sqliteInt.h
	$(COMPILE)  util.c
pager.o:pager.c pager.h sqliteInt.h os.h
	$(COMPILE) pager.c

os.o: os.c hash.h hash.c sqliteInt.h
	$(COMPILE) os.c 

hash.o :hash.c hash.h
	$(COMPILE) hash.c

testPager.o :testPager.c pager.h
	$(COMPILE) testPager.c 
random.o:
	$(COMPILE) random.c

clean:
	rm testPager util.o os.o hash.o testPager.o pager.o random.o

