# The LLVM Compiler Infrastructure
## Concept of "Undef"
  + Undef of Type T is set consitingall defined values of T.
  + undef value is subset of Undef.
```
int happiness;
if(HasProgrammerDefinedValue) 
  happiness=-1
print(happiness);   // happiness is indefined.
```
this is equivalent of 
```
movl $-1,%edi
call print
```
What we want at IR level is
```
x = phi (-1, undef)
call print
```
## Concept of Poison 
  + Represents a value of signed overflow in LLVM IR
  + Special Value that represents violation of assumption 
  + Each operation either propogates poison or raise UB
  + Poison is defined by any (defined/undefined) value.

## Interesting Things about UNDEF and POISON
  + Undef and poison can fold to different defined value at each use.
```
y = load uninit_var  <<= undef
use1(y)
use2(y)

=>
use1(0)
use2(1)

z = INT_MAX < (INT_MAX +nsw 1)   <<= poison
use1(z)
use2(z)
=>
use1(0)
use2(1)
```

  + UNDEF values dont have propertives 
```
y = x * 2   if x is poison, y  = x + x, y holds poison
y = x * 2   if x is undef, y  = x + x  undef not applicable.
```
+ Poison is more undefined that UNDEF
```
y = x + undef , if x is poison then y is poison 
x = c ? undef : y , if y is poison for true c, x(undef) != y (poison)
Why does this makes sense? 
UNDEF is set, concentric circles with Inner circle is undef and outer circle being poison.
All poisons are undef, not all undef are poison.
```
+ Poison can not be used in uninitialized bitfields


### Summary
+ Undefined Behavirour   is strongest, Least Defined
+ Poison is deferred UB, Less defined
+ UNDEF, undefined values are set of values, defined values but range.
+ Most Defined Values 



1. Freeze Operation
+ it ensures that the value becomes a single, specific value, even if the original value was non-deterministic. This can help prevent the propagation of undefined behavior in the program.
+  Usecase 
  - Handling Undefined Behavior  by stopping propogation of undefined  values
  - Optimizations can make use of safe transformation using freeze
  - Helps in formal verification via reducing non-determination

```
y = x * 2   -> y = x + x
if x is undef then {0,2,4} -> {0,1,2,3} since undef each use can have different value.
```
+ Operator freeze of form `x1 = freeze x` , x1 turns deterministic 
+ With Freeze deterministic nature, some opts stopped working. e.g. SCEV did not understand freeze, so it stopped loop strength reduction. It was recovered by hoisting freeze out of loop (Patch)[https://reviews.llvm.org/D77523]

# Note:
+ Alive2 can used to detect undef 
