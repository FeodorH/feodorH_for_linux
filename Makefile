# Компилятор и флаги
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic
TARGET = kubsh
SRC = main.cpp

# Основная цель
all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

# Сборка deb-пакета
deb: $(TARGET)
	@echo "Building Debian package..."
	# Создаем структуру пакета в текущей директории
	rm -rf deb_build
	mkdir -p deb_build/kubsh/DEBIAN
	mkdir -p deb_build/kubsh/usr/bin
	# Копируем бинарник
	cp kubsh deb_build/kubsh/usr/bin/
	# Создаем control файл
	@echo "Package: kubsh" > deb_build/kubsh/DEBIAN/control
	@echo "Version: 1.0-1" >> deb_build/kubsh/DEBIAN/control
	@echo "Architecture: amd64" >> deb_build/kubsh/DEBIAN/control
	@echo "Maintainer: Feodor <main_user@example.com>" >> deb_build/kubsh/DEBIAN/control
	@echo "Depends: libc6" >> deb_build/kubsh/DEBIAN/control
	@echo "Section: utils" >> deb_build/kubsh/DEBIAN/control
	@echo "Priority: optional" >> deb_build/kubsh/DEBIAN/control
	@echo "Description: Custom shell with user management VFS" >> deb_build/kubsh/DEBIAN/control
	@echo " A custom shell implementation that provides virtual file system" >> deb_build/kubsh/DEBIAN/control
	@echo " for user management with automatic directory monitoring." >> deb_build/kubsh/DEBIAN/control
	# Создаем postinst скрипт
	@echo "#!/bin/sh" > deb_build/kubsh/DEBIAN/postinst
	@echo "set -e" >> deb_build/kubsh/DEBIAN/postinst
	@echo "echo 'kubsh 1.0 installed successfully!'" >> deb_build/kubsh/DEBIAN/postinst
	@echo "echo 'Run \"kubsh\" to start the custom shell.'" >> deb_build/kubsh/DEBIAN/postinst
	chmod 755 deb_build/kubsh/DEBIAN/postinst
	# Собираем .deb пакет с именем kubsh.deb
	dpkg-deb --build deb_build/kubsh kubsh.deb
	# Очистка временных файлов
	rm -rf deb_build
	@echo "✅ Debian package created: kubsh.deb"
	@echo "📦 Install with: sudo dpkg -i kubsh.deb"

# Очистка
clean:
	rm -f $(TARGET)
	rm -f kubsh.deb
	rm -rf deb_build

# Установка (локально)
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/

.PHONY: all clean install deb
