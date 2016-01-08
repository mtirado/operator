# some global defines
DEFINES := 								\
			-DOPHOST_MAXHANDSHAKES=100			\
			-DOPHOST_MAXACCEPT=10				\
			-DOPHOST_MAXNAME=64				\
			-DOPHOST_HSHKDELAY=5000				\
			-DOPHOST_PINGDELAY=5000				\
			-DOP_REQ_PATH=\"/podhome/optest/request\"	\
			-DOP_REG_PATH=\"/podhome/optest/register\"
#			-DOP_REQ_PATH=\"/run/operator/request\"		\
#			-DOP_REG_PATH=\"/run/operator/register\"

CFLAGS  := -pedantic -Wall -Wextra -Werror $(DEFINES)
#-rdynamic: backtrace names
#LDFLAGS := -rdynamic
DEFLANG	:= -ansi
#DBG	:= -g

#TODO strip debugging info from binaries
#########################################
#	PROGRAM SOURCE FILES XXX ophost needed??
#########################################
OPERATOR_SRCS :=					\
		./operator.c			\
		./lib/ophost.c				\
		./eslib/eslib_file.c		\
		./eslib/eslib_sock.c
OPERATOR_OBJS := $(OPERATOR_SRCS:.c=.o)


########################################
#	TESTS
########################################
TEST_OPERATOR_SRCS :=					\
		./tests/operator_test.c		\
		./eslib/eslib_sock.c		\
		./eslib/eslib_file.c		\
		./eslib/eslib_proc.c		\
		./lib/ophost.c
TEST_OPERATOR_OBJS := $(TEST_OPERATOR_SRCS:.c=.o)


########################################
#	PROGRAM FILENAMES
########################################
OPERATOR 	:= operator
TEST_OPERATOR	:= operator_test

%.o: 		%.c
			$(CC) -c $(DEFLANG) $(CFLAGS) $(DBG) -o $@ $<

all:	$(OPERATOR)		\
	$(TEST_OPERATOR)



########################################
#	BUILD TARGETS
########################################
$(OPERATOR):		$(OPERATOR_OBJS)
		  	$(CC) $(LDFLAGS) $(OPERATOR_OBJS) -o $@
			@echo ""
			@echo "x----------------------------x"
			@echo "|        operator       OK   |"
			@echo "x----------------------------x"

$(TEST_OPERATOR):	$(TEST_OPERATOR_OBJS)
		  	$(CC) $(LDFLAGS) $(TEST_OPERATOR_OBJS) -o $@
			@echo ""
			@echo "x----------------------------x"
			@echo "|        operator_test  OK   |"
			@echo "x----------------------------x"


########################################
#	CLEAN UP THE MESS
########################################
clean:
	@$(foreach obj, $(OPERATOR_OBJS), rm -fv $(obj);)
	@$(foreach obj, $(TEST_OPERATOR_OBJS), rm -fv $(obj);)

	@-rm -fv ./$(OPERATOR)
	@-rm -fv ./$(TEST_OPERATOR)
	@echo cleaned.





