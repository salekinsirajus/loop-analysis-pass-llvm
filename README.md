# LLVM Analysis Pass: Loop Metadata
This pass analyzes and report a few aspects of a Loop. Specifically, this pass: 

- Adds metadata to the IR branch instruction representing the back edge for each
loop
- Adds metadata to the IR instruction that generates the loop induction variable
update used for loop termination check.

NOTE: in order to get the filename and line number, make sure you run the
compilation process with `-g` (debugging) enabled. 

## Use the Docker Cotnainer
Build a docker container using the docker file provided. If you already have the
prerequisite llvm-8 toolchain installed, you can skip to the next section.
```
docker build -t llvmdocker:latest .
```
Get into the docker container
```
docker run -v $(pwd):/llvmpass -it llvmdocker:latest /bin/bash
```

Follow the next set of instruciton now.

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
/usr/bin/opt-8 -load /llvmpass/build/libLoopMetaDataPass.so -loop-metadata -disable-output test.bc 
```
