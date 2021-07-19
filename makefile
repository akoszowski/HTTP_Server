CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -std=c++17

SRCS = http_server.cpp err.cpp
OBJS = ${SRCS:.cpp=.o}
HDRS = err.h

MAIN = server

all: ${MAIN}
	@echo Compiling http_server.

${MAIN}: ${OBJS}
	${CXX} ${CXXFLAGS} ${OBJS} -o ${MAIN} -lstdc++fs

.cpp.o:
	${CXX} ${CXXFLAGS} -c $< -o $@ -lstdc++fs

clean:
	${RM} ${MAIN} ${OBJS}

