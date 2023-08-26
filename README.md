# LLVM Analysis Pass: Loop Metadata
This pass analyzes and report a few aspects of a Loop. Specifically, this pass: 

- Adds metadata to the IR branch instruction representing the back edge for each
loop
- Adds metadata to the IR instruction that generates the loop induction variable
update used for loop termination check.

## Build From Source
1. Locate the directory where the source file is (make sure `CMakeLists.txt` is
   there)
2. Create a new directory with `mkdir build` and go in there `cd build`
3. Build the pass (while in `build`)
```
cmake ..
```
4. Assuming you have an input file called `test.bc`, you can run the following:
```
opt -load /ece566/build/libLoopMetaDataPass.so -loop-metadata -disable-output test.bc 
```

## Use the Docker Cotnainer
Build a docker container using the docker file provided.
```
docker build -t llvmp3:latest .
```
Get into the docker container
```
docker run -v $(pwd):/ece566 -it llvmp3:latest /bin/bash
```
Create a build directory
```
mkdir build
```
Run cmake
```
cmake .
```
Now run make
```
make
```
