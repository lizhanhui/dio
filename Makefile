dio:main.cc
	c++ -o dio -O2 main.cc -luring -lpthread -lgflags

clean:
	rm -rf dio