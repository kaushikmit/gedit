INCLUDES = -g -I$(top_srcdir) -I$(top_srcdir)/gedit $(GEDIT_DEBUG_FLAGS) $(GEDIT_CFLAGS)

noinst_PROGRAMS = $(TEST_PROGS)
progs_ldadd     = $(top_builddir)/gedit/libgedit.la


TEST_PROGS			= smart-converter
smart_converter_SOURCES		= smart-converter.c
smart_converter_LDADD		= $(progs_ldadd)

