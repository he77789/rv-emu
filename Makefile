CXX = g++-12
CXXFLAGS += -std=c++20
#CXXFLAGS += -Wno-unused-variable -Wno-unused-but-set-variable
CXXFLAGS += -Wall -Wextra -pedantic
CXXFLAGS += -Og -g

LDFLAGS += -fuse-ld=mold

BD = build/
objects = cpu.o mem.o io.o hartexc.o main.o uart.o aclint.o plic.o virtio_mmio_blk.o
OBJS := $(objects:%=$(BD)/%)

main: $(OBJS)
	$(CXX) $(LDFLAGS) $^ $(LOADLIBES) $(LDLIBS) -o $@

$(OBJS): $(BD)/%.o: %.cpp
	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

.PHONY: clean rebuild

clean:
	-rm $(BD)/*.o

rebuild: | clean main
