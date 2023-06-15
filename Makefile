dio:main.c
	gcc -o dio main.c -luring -lpthread

clean:
	rm -rf dio