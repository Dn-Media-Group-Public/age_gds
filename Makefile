all:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build -j$(shell nproc)

install:
	cmake --install build

clean:
	rm -rf build
