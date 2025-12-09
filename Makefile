CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
TARGET = kubsh
SRCS = main.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 4755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)

deb:
	mkdir -p debian/usr/bin
	cp $(TARGET) debian/usr/bin/
	
	mkdir -p debian/DEBIAN
	cat > debian/DEBIAN/control << EOF
Package: kubsh
Version: 1.0-1
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.34), adduser
Maintainer: Test User <test@example.com>
Description: Custom shell
Custom shell with VFS user management.
EOF
	
	cat > debian/DEBIAN/postinst << EOF
#!/bin/sh
set -e
chown root:root /usr/bin/kubsh
chmod 4755 /usr/bin/kubsh
EOF
	chmod 755 debian/DEBIAN/postinst
	
	dpkg-deb --build debian kubsh.deb

.PHONY: all clean install deb
