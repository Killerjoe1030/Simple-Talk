all: s-talk

s-talk: list.o s-talk.o
		gcc -o s-talk s-talk.o list.o


s-talk.o: s-talk.c
		gcc -c s-talk.c

clean:
		rm -f s-talk*.o  s-talk