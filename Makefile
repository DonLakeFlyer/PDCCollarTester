BUILD_DIR := build

.PHONY: all clean

all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(shell sysctl -n hw.ncpu)
	@echo "\nBuild complete: $(BUILD_DIR)/collar_tester"

install: all
	@echo "Building CollarTest.app..."
	@osacompile -o /Applications/CollarTest.app CollarTest.applescript
	@cp $(BUILD_DIR)/collar_tester /Applications/CollarTest.app/Contents/MacOS/
	@echo "Installed: /Applications/CollarTest.app"

clean:
	@rm -rf $(BUILD_DIR)
