$(call add-hdrs,fd_fuzz_demo.h)
$(call add-objs,fd_fuzz_demo,fd_util)
$(call fuzz-test,fuzz_fuzz_demo,fuzz_fuzz_demo,fd_util)
