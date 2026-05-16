# E-book Parser Wrappers Makefile
# 编译 libmobi + mupdf 的 C++ Wrapper 为 .so 共享库

# 配置
OUTPUT_DIR = ../libs
LIBMOBI_SRC = /opt/libmobi
MUPDF_SRC = /opt/mupdf

.PHONY: all mobi pdf clean

all: mobi pdf

# === libmobi wrapper ===
mobi:
	@echo "编译 MOBI/AZW3 wrapper..."
	mkdir -p $(OUTPUT_DIR)
	g++ -shared -fPIC -std=c++17 \
	  -I$(LIBMOBI_SRC)/src \
	  mobi_wrapper.cpp \
	  $(LIBMOBI_SRC)/src/.libs/libmobi.a \
	  -L/usr/lib/x86_64-linux-gnu -lxml2 -lz \
	  -o $(OUTPUT_DIR)/libmobiparse.so \
	  -lpthread -ldl
	@echo "完成: $(OUTPUT_DIR)/libmobiparse.so"

# === mupdf wrapper ===
pdf:
	@echo "编译 PDF wrapper..."
	mkdir -p $(OUTPUT_DIR)
	g++ -shared -fPIC -std=c++17 \
	  -I$(MUPDF_SRC)/include \
	  pdf_wrapper.cpp \
	  $(MUPDF_SRC)/build/release/libmupdf.a \
	  $(MUPDF_SRC)/build/release/libmupdf-third.a \
	  -lz -lm -lpthread -ldl \
	  -o $(OUTPUT_DIR)/libpdfparse.so
	@echo "完成: $(OUTPUT_DIR)/libpdfparse.so"

clean:
	rm -f $(OUTPUT_DIR)/libmobiparse.so
	rm -f $(OUTPUT_DIR)/libpdfparse.so
	@echo "已清理"
