# 在设备上原生编译（Debian arm64，gcc 14.2）
# 注意：isp_lib.c / isp_front.c 用 OpenMP（omp_get_wtime + #pragma omp），必须带 -fopenmp
#   make            产出主程序
#   make tools      产出测试工具
#   make install    装到 /usr/local/bin（需要 root）

CC      := gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -pthread -fopenmp -I$(CURDIR)
LDLIBS  ?= -lm -pthread -fopenmp
# 注：1) isp_lib.c/isp_front.c 用 OpenMP，必须 -fopenmp；2) tools/isptest.c 在 tools/ 下引用 isp_lib.h，需 -I$(CURDIR)
PREFIX  ?= /usr/local/bin

PROGS  = camrec5 camrec6 camvid camfront camisp
TOOLS  = camfeedfmt camfeedstat v4ltest isptest

all: $(PROGS)

tools: $(TOOLS)

camrec5: camrec5.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

camrec6: camrec6.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# camvid 内嵌 ISP 库
camvid: camvid.c isp_lib.c isp_lib.h
	$(CC) $(CFLAGS) -o $@ camvid.c isp_lib.c $(LDLIBS)

# camfront 用前置 ISP 库
camfront: camfront.c isp_front.c isp_lib.h
	$(CC) $(CFLAGS) -o $@ camfront.c isp_front.c $(LDLIBS)

camisp: camisp.c isp_lib.c isp_lib.h
	$(CC) $(CFLAGS) -o $@ camisp.c isp_lib.c $(LDLIBS)

camfeedfmt: tools/camfeedfmt.c
	$(CC) $(CFLAGS) -o $@ $<

camfeedstat: tools/camfeedstat.c
	$(CC) $(CFLAGS) -o $@ $<

v4ltest: tools/v4ltest.c
	$(CC) $(CFLAGS) -o $@ $<

isptest: tools/isptest.c isp_lib.c isp_lib.h
	$(CC) $(CFLAGS) -o $@ tools/isptest.c isp_lib.c $(LDLIBS)

install: all
	install -m 755 $(PROGS) $(PREFIX)/

clean:
	rm -f $(PROGS) $(TOOLS)

.PHONY: all tools install clean