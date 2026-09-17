if(CONFIG_EXAMPLES_TOUCH_DEMO)
  nuttx_add_application(NAME touch_demo SRCS touch_demo.c STACKSIZE 4096 PRIORITY 100)
endif()
