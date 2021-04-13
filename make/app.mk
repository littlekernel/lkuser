# shared make file for different applications

_APP_CSRCS := $(filter %.c,$(APP_SRCS))
_APP_CPPSRCS := $(filter %.cpp,$(APP_SRCS))
_APP_ASMSRCS := $(filter %.S,$(APP_SRCS))

#$(warning _APP_CSRCS = $(_APP_CSRCS))
#$(warning _APP_CPPSRCS = $(_APP_CPPSRCS))
#$(warning _APP_ASMSRCS = $(_APP_ASMSRCS))

_APP_COBJS := $(call TOBUILDDIR,$(patsubst %.c,%.o,$(_APP_CSRCS)))
_APP_CPPOBJS := $(call TOBUILDDIR,$(patsubst %.cpp,%.o,$(_APP_CPPSRCS)))
_APP_ASMOBJS := $(call TOBUILDDIR,$(patsubst %.S,%.o,$(_APP_ASMSRCS)))

#$(warning _APP_COBJS = $(_APP_COBJS))
#$(warning _APP_CPPOBJS = $(_APP_CPPOBJS))
#$(warning _APP_ASMOBJS = $(_APP_ASMOBJS))

_APP_OBJS := $(_APP_COBJS) $(_APP_CPPOBJS) $(_APP_ASMOBJS) $(_APP_ARM_COBJS) $(_APP_ARM_CPPOBJS) $(_APP_ARM_ASMOBJS)

#$(warning _APP_OBJS = $(_APP_OBJS))

$(_APP_COBJS): $(BUILDDIR)/%.o: %.c $(LIBC)
	@$(MKDIR)
	@echo compiling $<
	$(NOECHO)$(ARCH_CC) $(GLOBAL_OPTFLAGS) $(APP_OPTFLAGS) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(APP_COMPILEFLAGS) $(GLOBAL_CFLAGS) $(ARCH_CFLAGS) $(APP_CFLAGS) $(THUMBCFLAGS) $(GLOBAL_INCLUDES) $(APP_INCLUDES) -c $< -MD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(APP): _APP_OBJS := $(_APP_OBJS)
$(APP) $(APP).lst: $(_APP_OBJS) $(ARCH_LINKER_SCRIPT) $(GLOBAL_LIBS)
	@$(MKDIR)
	@echo linking $@
	$(NOECHO)$(ARCH_LD) $(GLOBAL_LDFLAGS) -T$(ARCH_LINKER_SCRIPT) $(ARCH_LDFLAGS) $(_APP_OBJS) $(EXTRA_OBJS) $(GLOBAL_LIBS) $(LIBGCC) -o $@
	@echo generating $@.strip
	$(NOECHO)$(ARCH_STRIP) -d $@ -o $@.strip
	@echo generating $@.lst
	$(NOECHO)$(ARCH_OBJDUMP) -d -r $@ > $@.lst
	@echo generating $@.dump
	$(NOECHO)$(ARCH_OBJDUMP) -x $@ > $@.dump

# add ourself to the build list
APPS += $(APP)

# empty out various local variables
_APP_CSRCS :=
_APP_CPPSRCS :=
_APP_ASMSRCS :=
_APP_COBJS :=
_APP_CPPOBJS :=
_APP_ASMOBJS :=
_APP_OBJS :=
APP :=

