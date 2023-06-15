dio:main.cc
	c++ -o dio -O2 main.cc -luring -lpthread

clean:
	rm -rf dio