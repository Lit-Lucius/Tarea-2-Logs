# Makefile para la Tarea 2 (Arboles B y B-epsilon)
# Uso:
#   make            # compila ./tarea2
#   make selftest   # compila y ejecuta las pruebas
#   make clean

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pedantic
TARGET    = tarea2
SRCS      = src/btree.cpp src/main.cpp
HEADERS   = src/common.hpp src/btree.hpp src/beps.hpp

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

selftest: $(TARGET)
	./$(TARGET) selftest

clean:
	rm -f $(TARGET) selftest_*.bin stress_*.bin

.PHONY: all selftest clean
