# shared make file for different applications

_LIB_CSRCS := $(filter %.c,$(LIB_SRCS))
_LIB_CPPSRCS := $(filter %.cpp,$(LIB_SRCS))
_LIB_ASMSRCS := $(filter %.S,$(LIB_SRCS))

#$(warning _LIB_CSRCS = $(_LIB_CSRCS))
#$(warning _LIB_CPPSRCS = $(_LIB_CPPSRCS))
#$(warning _LIB_ASMSRCS = $(_LIB_ASMSRCS))

_LIB_COBJS := $(call TOBUILDDIR,$(patsubst %.c,%.o,$(_LIB_CSRCS)))
_LIB_CPPOBJS := $(call TOBUILDDIR,$(patsubst %.cpp,%.o,$(_LIB_CPPSRCS)))
_LIB_ASMOBJS := $(call TOBUILDDIR,$(patsubst %.S,%.o,$(_LIB_ASMSRCS)))

#$(warning _LIB_COBJS = $(_LIB_COBJS))
#$(warning _LIB_CPPOBJS = $(_LIB_CPPOBJS))
#$(warning _LIB_ASMOBJS = $(_LIB_ASMOBJS))

_LIB_OBJS := $(_LIB_COBJS) $(_LIB_CPPOBJS) $(_LIB_ASMOBJS) $(_LIB_ARM_COBJS) $(_LIB_ARM_CPPOBJS) $(_LIB_ARM_ASMOBJS)

#$(warning _LIB_OBJS = $(_LIB_OBJS))

$(_LIB_COBJS): $(BUILDDIR)/%.o: %.c $(LIBC)
	@$(MKDIR)
	@echo compiling $<
	$(NOECHO)$(ARCH_CC) $(GLOBAL_OPTFLAGS) $(LIB_OPTFLAGS) $(GLOBAL_COMPILEFLAGS) $(ARCH_COMPILEFLAGS) $(LIB_COMPILEFLAGS) $(GLOBAL_CFLAGS) $(ARCH_CFLAGS) $(LIB_CFLAGS) $(THUMBCFLAGS) $(GLOBAL_INCLUDES) $(LIB_INCLUDES) -c $< -MD -MP -MT $@ -MF $(@:%o=%d) -o $@

$(LIB): _LIB_OBJS := $(_LIB_OBJS)
$(LIB): $(_LIB_OBJS)
	@$(MKDIR)
	@echo linking $@
	$(NOECHO)$(ARCH_AR) -rcs $@ $(_LIB_OBJS)

# add ourself to the build list
LIBS += $(LIB)

# empty out various local variables
_LIB_CSRCS :=
_LIB_CPPSRCS :=
_LIB_ASMSRCS :=
_LIB_COBJS :=
_LIB_CPPOBJS :=
_LIB_ASMOBJS :=
_LIB_OBJS :=
LIB_SRCS :=
LIB_OPTFLAGS :=
LIB_CFLAGS :=
LIB_COMPILEFLAGS :=
LIB_INCLUDES :=
LIB :=

