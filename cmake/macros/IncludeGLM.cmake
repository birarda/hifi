MACRO(INCLUDE_GLM TARGET)
	set(GLM_ROOT_DIR ../externals)
	find_package(GLM REQUIRED)
	include_directories(${GLM_INCLUDE_DIRS})
ENDMACRO(INCLUDE_GLM _target)