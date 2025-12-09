CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -pthread
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
	install -m 4755 $(TARGET) $(DESTDIR)/usr/bin/$(TARGET)  # УСТАНОВКА SETUID BIT!

deb:
	# Создаем структуру пакета
	mkdir -p debian/DEBIAN
	mkdir -p debian/usr/bin
	
	# Копируем бинарник
	cp $(TARGET) debian/usr/bin/
	
	# Создаем control файл для пакета
	cat > debian/DEBIAN/control << EOF
Package: kubsh
Version: 1.0-1
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.34), adduser
Maintainer: Your Name <your.email@example.com>
Description: Custom shell with VFS functionality
 A custom shell that monitors user directories and manages system users.
EOF
	
	# Устанавливаем права в postinst скрипте
	cat > debian/DEBIAN/postinst << EOF
#!/bin/bash
set -e
chown root:root /usr/bin/kubsh
chmod 4755 /usr/bin/kubsh
EOF
	chmod 755 debian/DEBIAN/postinst
	
	# Создаем пакет
	dpkg-deb --build debian kubsh.deb

.PHONY: all clean install deb
