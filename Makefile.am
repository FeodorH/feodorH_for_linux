CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = kubsh
SOURCES = src/main.cpp
OBJS = $(SOURCES:.cpp=.o)
PKG_NAME = kubsh
PKG_VERSION = 1.0
ARCH = amd64
BUILD_DIR = deb_build
DEB_FILE = kubsh.deb

# Основные цели
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
	rm -rf $(BUILD_DIR)
	rm -f $(DEB_FILE) $(PKG_NAME)_*.deb

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/

# Цель для сборки Debian пакета
deb: $(TARGET)
	@echo "Building Debian package..."
	@# Создаем структуру пакета в текущей директории
	@rm -rf $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/$(PKG_NAME)/DEBIAN
	@mkdir -p $(BUILD_DIR)/$(PKG_NAME)/usr/bin
	
	@# Копируем бинарник
	@cp $(TARGET) $(BUILD_DIR)/$(PKG_NAME)/usr/bin/
	
	@# Создаем control файл
	@echo "Package: $(PKG_NAME)" > $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Version: $(PKG_VERSION)-1" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Architecture: $(ARCH)" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Maintainer: Feodor <main_user@example.com>" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Depends: libc6" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Section: utils" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Priority: optional" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo "Description: Custom shell with user management VFS" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo " A custom shell implementation that provides virtual file system" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	@echo " for user management with automatic directory monitoring." >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/control
	
	@# Создаем postinst скрипт
	@echo "#!/bin/sh" > $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/postinst
	@echo "set -e" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/postinst
	@echo "echo '$(PKG_NAME) $(PKG_VERSION) installed successfully!'" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/postinst
	@echo "echo 'Run \\\"$(TARGET)\\\" to start the custom shell.'" >> $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/postinst
	@chmod 755 $(BUILD_DIR)/$(PKG_NAME)/DEBIAN/postinst
	
	@# Собираем .deb пакет с именем kubsh.deb
	@dpkg-deb --build $(BUILD_DIR)/$(PKG_NAME) $(DEB_FILE)
	
	@# Очистка временных файлов
	@rm -rf $(BUILD_DIR)
	
	@echo "✅ Debian package created: $(DEB_FILE)"
	@echo "📦 Install with: sudo dpkg -i $(DEB_FILE)"

# Проверка пакета
check-deb:
	@if [ -f "$(DEB_FILE)" ]; then \
		echo "=== Package info ==="; \
		dpkg -I $(DEB_FILE); \
		echo "=== Package contents ==="; \
		dpkg -c $(DEB_FILE); \
	else \
		echo "Package not found. Run 'make deb' first."; \
	fi

# Установка пакета (локальная тестовая)
install-deb: deb
	@echo "Installing package..."
	@sudo dpkg -i $(DEB_FILE) || true
	@sudo apt-get install -f -y

# Удаление пакета
uninstall-deb:
	@sudo dpkg -r $(PKG_NAME) || true

.PHONY: all clean install deb check-deb install-deb uninstall-deb
