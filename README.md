# LLVM Analysis Pass: Loop 
This pass analyzes and report a few aspects of a Loop. Specifically, this pass: 

- Adds metadata to the IR branch instruction representing the back edge for each
loop
- Adds metadata to the IR instruction that generates the loop induction variable
update used for loop termination check.

## Acceptance Criteria
3. All Metadata should include corresponding filename and line number in the C++ code
4. Test case may include 2 or 3 level embedded loops as well as multiple loops at same
level.
5. Pass source code should be compilable at our end using llvm 8.0 version.
6. The pass should define any dependencies on other passes automatically. It is not
expected that we run multiple llvm clang and/or opt passes prior to running this pass.
The output of the pass is an .ll file. It is expected that this Meta-data annotated IR should
be able to compile to executable and be functional. There should be no functional
difference in executable when compiled with and without this pass.

## Running Locally
Build a docker container using the docker file provided.
```
docker build -t llvmp3:latest .
```

### Build Executable
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
cmake ..
```
Now run make
```
make
```

## Building Test
Create a test directory in the root folder and `cd` into it
```
mkdir test
cd test
```
Configure the benchmark tool to find the executable that we built earlier
```
/ece566/wolfbench/wolfbench/configure --enable-customtool=/ece566/build/p3
```

## Running Test
while you are in the `test` directory, run the following command to compile
all the benchmark.
```
make all test
```
To ensure that our optimization pass did not break things, run the following:
```
make test compare
```
