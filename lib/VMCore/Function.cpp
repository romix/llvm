//===-- Function.cpp - Implement the Global object classes ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Function class for the VMCore library.
//
//===----------------------------------------------------------------------===//

#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/LeakDetector.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/StringPool.h"
#include "llvm/Support/RWMutex.h"
#include "llvm/Support/Threading.h"
#include "SymbolTableListTraitsImpl.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
using namespace llvm;

// Explicit instantiations of SymbolTableListTraits since some of the methods
// are not in the public header file...
template class llvm::SymbolTableListTraits<Argument, Function>;
template class llvm::SymbolTableListTraits<BasicBlock, Function>;

//===----------------------------------------------------------------------===//
// Argument Implementation
//===----------------------------------------------------------------------===//

void Argument::anchor() { }

Argument::Argument(Type *Ty, const Twine &Name, Function *Par)
  : Value(Ty, Value::ArgumentVal) {
  Parent = 0;

  // Make sure that we get added to a function
  LeakDetector::addGarbageObject(this);

  if (Par)
    Par->getArgumentList().push_back(this);
  setName(Name);
}

void Argument::setParent(Function *parent) {
  if (getParent())
    LeakDetector::addGarbageObject(this);
  Parent = parent;
  if (getParent())
    LeakDetector::removeGarbageObject(this);
}

/// getArgNo - Return the index of this formal argument in its containing
/// function.  For example in "void foo(int a, float b)" a is 0 and b is 1. 
unsigned Argument::getArgNo() const {
  const Function *F = getParent();
  assert(F && "Argument is not in a function");
  
  Function::const_arg_iterator AI = F->arg_begin();
  unsigned ArgIdx = 0;
  for (; &*AI != this; ++AI)
    ++ArgIdx;

  return ArgIdx;
}

/// hasByValAttr - Return true if this argument has the byval attribute on it
/// in its containing function.
bool Argument::hasByValAttr() const {
  if (!getType()->isPointerTy()) return false;
  return getParent()->paramHasAttr(getArgNo()+1, Attribute::ByVal);
}

unsigned Argument::getParamAlignment() const {
  assert(getType()->isPointerTy() && "Only pointers have alignments");
  return getParent()->getParamAlignment(getArgNo()+1);
  
}

/// hasNestAttr - Return true if this argument has the nest attribute on
/// it in its containing function.
bool Argument::hasNestAttr() const {
  if (!getType()->isPointerTy()) return false;
  return getParent()->paramHasAttr(getArgNo()+1, Attribute::Nest);
}

/// hasNoAliasAttr - Return true if this argument has the noalias attribute on
/// it in its containing function.
bool Argument::hasNoAliasAttr() const {
  if (!getType()->isPointerTy()) return false;
  return getParent()->paramHasAttr(getArgNo()+1, Attribute::NoAlias);
}

/// hasNoCaptureAttr - Return true if this argument has the nocapture attribute
/// on it in its containing function.
bool Argument::hasNoCaptureAttr() const {
  if (!getType()->isPointerTy()) return false;
  return getParent()->paramHasAttr(getArgNo()+1, Attribute::NoCapture);
}

/// hasSRetAttr - Return true if this argument has the sret attribute on
/// it in its containing function.
bool Argument::hasStructRetAttr() const {
  if (!getType()->isPointerTy()) return false;
  if (this != getParent()->arg_begin())
    return false; // StructRet param must be first param
  return getParent()->paramHasAttr(1, Attribute::StructRet);
}

/// addAttr - Add a Attribute to an argument
void Argument::addAttr(Attributes attr) {
  getParent()->addAttribute(getArgNo() + 1, attr);
}

/// removeAttr - Remove a Attribute from an argument
void Argument::removeAttr(Attributes attr) {
  getParent()->removeAttribute(getArgNo() + 1, attr);
}


//===----------------------------------------------------------------------===//
// Helper Methods in Function
//===----------------------------------------------------------------------===//

LLVMContext &Function::getContext() const {
  return getType()->getContext();
}

FunctionType *Function::getFunctionType() const {
  return cast<FunctionType>(getType()->getElementType());
}

bool Function::isVarArg() const {
  return getFunctionType()->isVarArg();
}

Type *Function::getReturnType() const {
  return getFunctionType()->getReturnType();
}

void Function::removeFromParent() {
  getParent()->getFunctionList().remove(this);
}

void Function::eraseFromParent() {
  getParent()->getFunctionList().erase(this);
}

//===----------------------------------------------------------------------===//
// Function Implementation
//===----------------------------------------------------------------------===//

Function::Function(FunctionType *Ty, LinkageTypes Linkage,
                   const Twine &name, Module *ParentModule)
  : GlobalValue(PointerType::getUnqual(Ty), 
                Value::FunctionVal, 0, 0, Linkage, name) {
  assert(FunctionType::isValidReturnType(getReturnType()) &&
         "invalid return type");
  SymTab = new ValueSymbolTable();

  // If the function has arguments, mark them as lazily built.
  if (Ty->getNumParams())
    setValueSubclassData(1);   // Set the "has lazy arguments" bit.
  
  // Make sure that we get added to a function
  LeakDetector::addGarbageObject(this);

  if (ParentModule)
    ParentModule->getFunctionList().push_back(this);

  // Ensure intrinsics have the right parameter attributes.
  if (unsigned IID = getIntrinsicID())
    setAttributes(Intrinsic::getAttributes(Intrinsic::ID(IID)));

}

Function::~Function() {
  dropAllReferences();    // After this it is safe to delete instructions.

  // Delete all of the method arguments and unlink from symbol table...
  ArgumentList.clear();
  delete SymTab;

  // Remove the function from the on-the-side GC table.
  clearGC();
}

void Function::BuildLazyArguments() const {
  // Create the arguments vector, all arguments start out unnamed.
  FunctionType *FT = getFunctionType();
  for (unsigned i = 0, e = FT->getNumParams(); i != e; ++i) {
    assert(!FT->getParamType(i)->isVoidTy() &&
           "Cannot have void typed arguments!");
    ArgumentList.push_back(new Argument(FT->getParamType(i)));
  }
  
  // Clear the lazy arguments bit.
  unsigned SDC = getSubclassDataFromValue();
  const_cast<Function*>(this)->setValueSubclassData(SDC &= ~1);
}

size_t Function::arg_size() const {
  return getFunctionType()->getNumParams();
}
bool Function::arg_empty() const {
  return getFunctionType()->getNumParams() == 0;
}

void Function::setParent(Module *parent) {
  if (getParent())
    LeakDetector::addGarbageObject(this);
  Parent = parent;
  if (getParent())
    LeakDetector::removeGarbageObject(this);
}

// dropAllReferences() - This function causes all the subinstructions to "let
// go" of all references that they are maintaining.  This allows one to
// 'delete' a whole class at a time, even though there may be circular
// references... first all references are dropped, and all use counts go to
// zero.  Then everything is deleted for real.  Note that no operations are
// valid on an object that has "dropped all references", except operator
// delete.
//
void Function::dropAllReferences() {
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->dropAllReferences();
  
  // Delete all basic blocks. They are now unused, except possibly by
  // blockaddresses, but BasicBlock's destructor takes care of those.
  while (!BasicBlocks.empty())
    BasicBlocks.begin()->eraseFromParent();
}

void Function::addAttribute(unsigned i, Attributes attr) {
  AttrListPtr PAL = getAttributes();
  PAL = PAL.addAttr(i, attr);
  setAttributes(PAL);
}

void Function::removeAttribute(unsigned i, Attributes attr) {
  AttrListPtr PAL = getAttributes();
  PAL = PAL.removeAttr(i, attr);
  setAttributes(PAL);
}

// Maintain the GC name for each function in an on-the-side table. This saves
// allocating an additional word in Function for programs which do not use GC
// (i.e., most programs) at the cost of increased overhead for clients which do
// use GC.
static DenseMap<const Function*,PooledStringPtr> *GCNames;
static StringPool *GCNamePool;
static ManagedStatic<sys::SmartRWMutex<true> > GCLock;

bool Function::hasGC() const {
  sys::SmartScopedReader<true> Reader(*GCLock);
  return GCNames && GCNames->count(this);
}

const char *Function::getGC() const {
  assert(hasGC() && "Function has no collector");
  sys::SmartScopedReader<true> Reader(*GCLock);
  return *(*GCNames)[this];
}

void Function::setGC(const char *Str) {
  sys::SmartScopedWriter<true> Writer(*GCLock);
  if (!GCNamePool)
    GCNamePool = new StringPool();
  if (!GCNames)
    GCNames = new DenseMap<const Function*,PooledStringPtr>();
  (*GCNames)[this] = GCNamePool->intern(Str);
}

void Function::clearGC() {
  sys::SmartScopedWriter<true> Writer(*GCLock);
  if (GCNames) {
    GCNames->erase(this);
    if (GCNames->empty()) {
      delete GCNames;
      GCNames = 0;
      if (GCNamePool->empty()) {
        delete GCNamePool;
        GCNamePool = 0;
      }
    }
  }
}

/// copyAttributesFrom - copy all additional attributes (those not needed to
/// create a Function) from the Function Src to this one.
void Function::copyAttributesFrom(const GlobalValue *Src) {
  assert(isa<Function>(Src) && "Expected a Function!");
  GlobalValue::copyAttributesFrom(Src);
  const Function *SrcF = cast<Function>(Src);
  setCallingConv(SrcF->getCallingConv());
  setAttributes(SrcF->getAttributes());
  if (SrcF->hasGC())
    setGC(SrcF->getGC());
  else
    clearGC();
}

/// getIntrinsicID - This method returns the ID number of the specified
/// function, or Intrinsic::not_intrinsic if the function is not an
/// intrinsic, or if the pointer is null.  This value is always defined to be
/// zero to allow easy checking for whether a function is intrinsic or not.  The
/// particular intrinsic functions which correspond to this value are defined in
/// llvm/Intrinsics.h.
///
unsigned Function::getIntrinsicID() const {
  const ValueName *ValName = this->getValueName();
  if (!ValName)
    return 0;
  unsigned Len = ValName->getKeyLength();
  const char *Name = ValName->getKeyData();
  
  if (Len < 5 || Name[4] != '.' || Name[0] != 'l' || Name[1] != 'l'
      || Name[2] != 'v' || Name[3] != 'm')
    return 0;  // All intrinsics start with 'llvm.'

#define GET_FUNCTION_RECOGNIZER
#include "llvm/Intrinsics.gen"
#undef GET_FUNCTION_RECOGNIZER
  return 0;
}

std::string Intrinsic::getName(ID id, ArrayRef<Type*> Tys) {
  assert(id < num_intrinsics && "Invalid intrinsic ID!");
  static const char * const Table[] = {
    "not_intrinsic",
#define GET_INTRINSIC_NAME_TABLE
#include "llvm/Intrinsics.gen"
#undef GET_INTRINSIC_NAME_TABLE
  };
  if (Tys.empty())
    return Table[id];
  std::string Result(Table[id]);
  for (unsigned i = 0; i < Tys.size(); ++i) {
    if (PointerType* PTyp = dyn_cast<PointerType>(Tys[i])) {
      Result += ".p" + llvm::utostr(PTyp->getAddressSpace()) + 
                EVT::getEVT(PTyp->getElementType()).getEVTString();
    }
    else if (Tys[i])
      Result += "." + EVT::getEVT(Tys[i]).getEVTString();
  }
  return Result;
}

#define GET_INTRINSTIC_GENERATOR_GLOBAL
#include "llvm/Intrinsics.gen"
#undef GET_INTRINSTIC_GENERATOR_GLOBAL

static Type *DecodeFixedType(unsigned &NextElt, ArrayRef<unsigned char> Infos,
                             ArrayRef<Type*> Tys, LLVMContext &Context) {
  IIT_Info Info = IIT_Info(Infos[NextElt++]);
  unsigned StructElts = 2;
  
  switch (Info) {
  case IIT_Done: return Type::getVoidTy(Context);
  case IIT_I1: return Type::getInt1Ty(Context);
  case IIT_I8: return Type::getInt8Ty(Context);
  case IIT_I16: return Type::getInt16Ty(Context);
  case IIT_I32: return Type::getInt32Ty(Context);
  case IIT_I64: return Type::getInt64Ty(Context);
  case IIT_F32: return Type::getFloatTy(Context);
  case IIT_F64: return Type::getDoubleTy(Context);
  case IIT_MMX: return Type::getX86_MMXTy(Context);
  case IIT_METADATA: return Type::getMetadataTy(Context);
  case IIT_EMPTYSTRUCT: return StructType::get(Context);
  case IIT_V2:
    return VectorType::get(DecodeFixedType(NextElt, Infos, Tys, Context), 2);
  case IIT_V4:
    return VectorType::get(DecodeFixedType(NextElt, Infos, Tys, Context), 4);
  case IIT_V8:
    return VectorType::get(DecodeFixedType(NextElt, Infos, Tys, Context), 8);
  case IIT_V16:
    return VectorType::get(DecodeFixedType(NextElt, Infos, Tys, Context), 16);
  case IIT_V32:
    return VectorType::get(DecodeFixedType(NextElt, Infos, Tys, Context), 32);
  case IIT_PTR:
    return PointerType::getUnqual(DecodeFixedType(NextElt, Infos, Tys,Context));
  case IIT_ARG:
  case IIT_EXTEND_VEC_ARG:
  case IIT_TRUNC_VEC_ARG: {
    unsigned ArgNo = NextElt == Infos.size() ? 0 : Infos[NextElt++];
    assert(ArgNo < Tys.size() && "Not enough types specified!");
    Type *T = Tys[ArgNo];
   
    if (Info == IIT_EXTEND_VEC_ARG)
      T = VectorType::getExtendedElementVectorType(cast<VectorType>(T));
    if (Info == IIT_TRUNC_VEC_ARG)
      T = VectorType::getTruncatedElementVectorType(cast<VectorType>(T));
    return T;
  }
  case IIT_STRUCT5: ++StructElts; // FALL THROUGH.
  case IIT_STRUCT4: ++StructElts; // FALL THROUGH.
  case IIT_STRUCT3: ++StructElts; // FALL THROUGH.
  case IIT_STRUCT2: {
    Type *Elts[5];
    for (unsigned i = 0; i != StructElts; ++i)
      Elts[i] = DecodeFixedType(NextElt, Infos, Tys, Context);
    return StructType::get(Context, ArrayRef<Type*>(Elts, StructElts));
  }
  }
  llvm_unreachable("unhandled");
}
  

FunctionType *Intrinsic::getType(LLVMContext &Context,
                                 ID id, ArrayRef<Type*> Tys) {
  // Check to see if the intrinsic's type was expressible by the table.
  unsigned TableVal = IIT_Table[id-1];
  
  // Decode the TableVal into an array of IITValues.
  SmallVector<unsigned char, 8> IITValues;
  ArrayRef<unsigned char> IITEntries;
  unsigned NextElt = 0;
  if ((TableVal >> 31) != 0) {
    // This is an offset into the IIT_LongEncodingTable.
    IITEntries = IIT_LongEncodingTable;
    
    // Strip sentinel bit.
    NextElt = (TableVal << 1) >> 1;
  } else {
    do {
      IITValues.push_back(TableVal & 0xF);
      TableVal >>= 4;
    } while (TableVal);
    
    IITEntries = IITValues;
    NextElt = 0;
  }
    
  Type *ResultTy = DecodeFixedType(NextElt, IITEntries, Tys, Context);
    
  SmallVector<Type*, 8> ArgTys;
  while (NextElt != IITEntries.size() && IITEntries[NextElt] != 0)
    ArgTys.push_back(DecodeFixedType(NextElt, IITEntries, Tys, Context));

  return FunctionType::get(ResultTy, ArgTys, false); 
}

bool Intrinsic::isOverloaded(ID id) {
#define GET_INTRINSIC_OVERLOAD_TABLE
#include "llvm/Intrinsics.gen"
#undef GET_INTRINSIC_OVERLOAD_TABLE
}

/// This defines the "Intrinsic::getAttributes(ID id)" method.
#define GET_INTRINSIC_ATTRIBUTES
#include "llvm/Intrinsics.gen"
#undef GET_INTRINSIC_ATTRIBUTES

Function *Intrinsic::getDeclaration(Module *M, ID id, ArrayRef<Type*> Tys) {
  // There can never be multiple globals with the same name of different types,
  // because intrinsics must be a specific type.
  return
    cast<Function>(M->getOrInsertFunction(getName(id, Tys),
                                          getType(M->getContext(), id, Tys)));
}

// This defines the "Intrinsic::getIntrinsicForGCCBuiltin()" method.
#define GET_LLVM_INTRINSIC_FOR_GCC_BUILTIN
#include "llvm/Intrinsics.gen"
#undef GET_LLVM_INTRINSIC_FOR_GCC_BUILTIN

/// hasAddressTaken - returns true if there are any uses of this function
/// other than direct calls or invokes to it.
bool Function::hasAddressTaken(const User* *PutOffender) const {
  for (Value::const_use_iterator I = use_begin(), E = use_end(); I != E; ++I) {
    const User *U = *I;
    if (isa<BlockAddress>(U))
      continue;
    if (!isa<CallInst>(U) && !isa<InvokeInst>(U))
      return PutOffender ? (*PutOffender = U, true) : true;
    ImmutableCallSite CS(cast<Instruction>(U));
    if (!CS.isCallee(I))
      return PutOffender ? (*PutOffender = U, true) : true;
  }
  return false;
}

bool Function::isDefTriviallyDead() const {
  // Check the linkage
  if (!hasLinkOnceLinkage() && !hasLocalLinkage() &&
      !hasAvailableExternallyLinkage())
    return false;

  // Check if the function is used by anything other than a blockaddress.
  for (Value::const_use_iterator I = use_begin(), E = use_end(); I != E; ++I)
    if (!isa<BlockAddress>(*I))
      return false;

  return true;
}

/// callsFunctionThatReturnsTwice - Return true if the function has a call to
/// setjmp or other function that gcc recognizes as "returning twice".
bool Function::callsFunctionThatReturnsTwice() const {
  for (const_inst_iterator
         I = inst_begin(this), E = inst_end(this); I != E; ++I) {
    const CallInst* callInst = dyn_cast<CallInst>(&*I);
    if (!callInst)
      continue;
    if (callInst->canReturnTwice())
      return true;
  }

  return false;
}

