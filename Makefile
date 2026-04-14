BUILD_DIR := build

.PHONY: all clean

all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(shell sysctl -n hw.ncpu)
	@echo "\nBuild complete: $(BUILD_DIR)/collar_tester"

clean:
	@rm -rf $(BUILD_DIR)
