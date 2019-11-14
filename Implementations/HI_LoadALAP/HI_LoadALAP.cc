#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "HI_print.h"
#include "HI_LoadALAP.h"
#include "llvm/Transforms/Utils/Local.h"
#include <stdio.h>
#include <string>
#include <ios>
#include <stdlib.h>

using namespace llvm;
 
bool HI_LoadALAP::runOnFunction(llvm::Function &F) // The runOnModule declaration will overide the virtual one in ModulePass, which will be executed for each Module.
{
    print_status("Running HI_LoadALAP pass."); 
    if (F.getName().find("llvm.") != std::string::npos)
        return false;
        
    // example:

    // a = load
    // b1 = a + b0
    // b2 = c1 + b1

    // after transform: 
    // (condition: b1,b2 has only one user)
    // (condition: c1 is not a load instruction)

    // a = load
    // b1 = c1 + b0
    // b2 = a + b1


    bool changed = 0;
    for (BasicBlock &B : F) 
    {
        bool action = 1;
        while (action)
        {
            action = 0;
            for (BasicBlock::reverse_iterator it=B.rbegin(), ie=B.rend(); it!=ie; it++) 
            {
                Instruction* I=&*it;

                if (generatedI.find(I) != generatedI.end()) // this Instruction in generated by us, bypass it
                    continue;

                if (I->getOpcode() == Instruction::Add)
                    action |= tryReorderIntAdd(I);

                else if (I->getOpcode() == Instruction::Mul)
                    action |= tryReorderIntMul(I);

                // else if (I->getOpcode() == Instruction::FAdd) // VivadoHLS does not has such optimization yet
                //                                               // disable it
                //     action |= tryReorderFloatAdd(I);

                // else if (I->getOpcode() == Instruction::FMul)
                //     action |= tryReorderFloatMul(I);

                if (action)
                    break;
                    
            }
        }

    }
    HI_LoadALAPLog->flush(); 
    return changed;
}



char HI_LoadALAP::ID = 0;  // the ID for pass should be initialized but the value does not matter, since LLVM uses the address of this variable as label instead of its value.

void HI_LoadALAP::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesCFG();
}

bool HI_LoadALAP::tryReorderIntAdd(Instruction *AddI)
{
    if (!(dyn_cast<AddOperator>(AddI->getOperand(0)) || dyn_cast<AddOperator>(AddI->getOperand(1))))
        return false;
    // find the operands of the ser
    std::vector<Value*> nonLoadInsts;
    std::vector<Value*> LoadInsts;
    recursiveGetAddOperand(AddI, nonLoadInsts, LoadInsts);

    if (LoadInsts.size() + nonLoadInsts.size()<3)
        return false;

    // merge them in order
    uint64_t constIntTot = 0;
    bool hasConst = false;

    for (auto nonLoadI : nonLoadInsts)
    {
      //  llvm::errs() << "   nonLoadI=" << *nonLoadI << "\n";
        if (auto constIntVal = dyn_cast<ConstantInt>(nonLoadI))
        {
            hasConst = true;
            constIntTot += *(constIntVal->getValue().getRawData());
        }
        else
        {
            LoadInsts.push_back(nonLoadI);
        } 
    }
    if (hasConst)
        LoadInsts.push_back(ConstantInt::get(AddI->getType(), constIntTot));



    if (DEBUG) *HI_LoadALAPLog << "Instruction: " << *AddI << " has following components:\n";
    if (DEBUG) 
    {
        if (hasConst)
            *HI_LoadALAPLog << "some consts can be merged:  " << constIntTot << "\n";
    }
    for (Value* tmp_val : LoadInsts)
    {
        if (DEBUG) *HI_LoadALAPLog << "      " << *tmp_val << "\n"; 
      //  llvm::errs() << "   tmp_val=" << *tmp_val << "\n";
    }

    if (DEBUG) *HI_LoadALAPLog << "\n the old block is:\n";
    if (DEBUG) *HI_LoadALAPLog << *AddI->getParent() << "\n==================================\n";

    
    std::vector<Value*> lastInstVec = LoadInsts;

    IRBuilder<> Builder(AddI);

    int numVal = lastInstVec.size();
    while (numVal != 1)
    {
        for (int i=0; i<numVal/2; i++)
        {
            Value *newAdd = Builder.CreateAdd(lastInstVec[2*i], lastInstVec[2*i+1]);
            generatedI.insert(newAdd);
            lastInstVec[i] = newAdd;
        }
        if (numVal%2)
        {
            lastInstVec[(numVal+1)/2-1] = lastInstVec[numVal-1];
        }
        numVal = (numVal+1)/2;
    }

    auto newAddAll = dyn_cast<Instruction>(lastInstVec[0]);
    auto newAddAll_C = dyn_cast<ConstantInt>(lastInstVec[0]);
    auto newAddAll_V = (lastInstVec[0]);

    assert(newAddAll || newAddAll_C);

    if (DEBUG) *HI_LoadALAPLog << "new add instruction generated: " << *newAddAll_V << " to replace " << *AddI << " and the new block is:\n";
    AddI->replaceAllUsesWith(newAddAll_V);


    // llvm::errs() << "   newAddAll_V=" << *newAddAll_V << "\n";

    RecursivelyDeleteTriviallyDeadInstructions(AddI);
    if (DEBUG) *HI_LoadALAPLog << "\n==================================\n\n";
    return true;        
}



void HI_LoadALAP::recursiveGetAddOperand(Value *AddI, std::vector<Value*> &nonLoadInsts, std::vector<Value*> &LoadInsts)
{
    if (auto real_AddI = dyn_cast<AddOperator>(AddI))
    {
        for (int i=0; i<real_AddI->getNumOperands(); i++)
        {
            AddOperator* opAdd = dyn_cast<AddOperator>(real_AddI->getOperand(i));
            if (opAdd)
            {
                recursiveGetAddOperand(opAdd, nonLoadInsts, LoadInsts);          
            }
            else
            {
                if (auto LoadI = dyn_cast<LoadInst>(real_AddI->getOperand(i)))
                {
                    LoadInsts.push_back(LoadI);
                }
                else
                {
                    nonLoadInsts.push_back(real_AddI->getOperand(i));
                }
            }   
        }
    }

}

bool HI_LoadALAP::tryReorderIntMul(Instruction *MulI)
{
    if (!(dyn_cast<MulOperator>(MulI->getOperand(0)) || dyn_cast<MulOperator>(MulI->getOperand(1))))
        return false;
    // find the operands of the ser
    std::vector<Value*> nonLoadInsts;
    std::vector<Value*> LoadInsts;
    recursiveGetMulOperand(MulI, nonLoadInsts, LoadInsts);

    if (LoadInsts.size() + nonLoadInsts.size()<3)
        return false;

    // merge them in order
    uint64_t constIntTot = 1;
    bool hasConst = false;

    for (auto nonLoadI : nonLoadInsts)
    {
      //  llvm::errs() << "   nonLoadI=" << *nonLoadI << "\n";
        if (auto constIntVal = dyn_cast<ConstantInt>(nonLoadI))
        {
            hasConst = true;
            constIntTot *= *(constIntVal->getValue().getRawData());
        }
        else
        {
            LoadInsts.push_back(nonLoadI);
        } 
    }
    if (hasConst)
        LoadInsts.push_back(ConstantInt::get(MulI->getType(), constIntTot));



    if (DEBUG) *HI_LoadALAPLog << "Instruction: " << *MulI << " has following components:\n";
    if (DEBUG) 
    {
        if (hasConst)
            *HI_LoadALAPLog << "some consts can be merged:  " << constIntTot << "\n";
    }
    for (Value* tmp_val : LoadInsts)
    {
        if (DEBUG) *HI_LoadALAPLog << "      " << *tmp_val << "\n"; 
      //  llvm::errs() << "   tmp_val=" << *tmp_val << "\n";
    }

    if (DEBUG) *HI_LoadALAPLog << "Instruction: " << *MulI << " has following components:\n";
    for (Value* tmp_val : LoadInsts)
    {
        if (DEBUG) *HI_LoadALAPLog << "      " << *tmp_val << "\n"; 
    }

    if (DEBUG) *HI_LoadALAPLog << "\n the old block is:\n";
    if (DEBUG) *HI_LoadALAPLog << *MulI->getParent() << "\n==================================\n";

    
    std::vector<Value*> lastInstVec = LoadInsts;

    IRBuilder<> Builder(MulI);

    int numVal = lastInstVec.size();
    while (numVal != 1)
    {
        for (int i=0; i<numVal/2; i++)
        {
            Value *newMul = Builder.CreateMul(lastInstVec[2*i], lastInstVec[2*i+1]);
            generatedI.insert(newMul);
            lastInstVec[i] = newMul;
        }
        if (numVal%2)
        {
            lastInstVec[(numVal+1)/2-1] = lastInstVec[numVal-1];
        }
        numVal = (numVal+1)/2;
    }

    auto newMulAll = dyn_cast<Instruction>(lastInstVec[0]);
    auto newMulAll_C = dyn_cast<ConstantInt>(lastInstVec[0]);
    auto newMulAll_V = (lastInstVec[0]);

    assert(newMulAll || newMulAll_C);

    if (DEBUG) *HI_LoadALAPLog << "new Mul instruction generated: " << *newMulAll_V << " to replace " << *MulI << " and the new block is:\n";
    MulI->replaceAllUsesWith(newMulAll_V);
    RecursivelyDeleteTriviallyDeadInstructions(MulI);
    if (DEBUG) *HI_LoadALAPLog << "\n==================================\n\n";
    return true;        
}



void HI_LoadALAP::recursiveGetMulOperand(Value *MulI, std::vector<Value*> &nonLoadInsts, std::vector<Value*> &LoadInsts)
{
    if (auto real_MulI = dyn_cast<MulOperator>(MulI))
    {
        for (int i=0; i<real_MulI->getNumOperands(); i++)
        {
            MulOperator* opMul = dyn_cast<MulOperator>(real_MulI->getOperand(i));
            if (opMul)
            {
                recursiveGetMulOperand(opMul, nonLoadInsts, LoadInsts);          
            }
            else
            {
                if (auto LoadI = dyn_cast<LoadInst>(real_MulI->getOperand(i)))
                {
                    LoadInsts.push_back(LoadI);
                }
                else
                {
                    nonLoadInsts.push_back(real_MulI->getOperand(i));
                }
            }   
        }
    }

}



bool HI_LoadALAP::tryReorderFloatAdd(Instruction *AddI)
{
    // if (!(dyn_cast<FPMathOperator>(AddI->getOperand(0)) || dyn_cast<FPMathOperator>(AddI->getOperand(1))))
    //     return false;
    // find the operands of the ser
    std::vector<Value*> nonLoadInsts;
    std::vector<Value*> LoadInsts;
    recursiveGetFAddOperand(AddI, nonLoadInsts, LoadInsts);

    // merge them in order
    for (auto LoadI : LoadInsts)
        nonLoadInsts.push_back(LoadI);

    if (nonLoadInsts.size()<3)
        return false;

    if (DEBUG) *HI_LoadALAPLog << "Instruction: " << *AddI << " has following components:\n";
    for (Value* tmp_val : nonLoadInsts)
    {
        if (DEBUG) *HI_LoadALAPLog << "      " << *tmp_val << "\n"; 
    }

    if (DEBUG) *HI_LoadALAPLog << "\n the old block is:\n";
    if (DEBUG) *HI_LoadALAPLog << *AddI->getParent() << "\n==================================\n";

    
    std::vector<Value*> lastInstVec = nonLoadInsts;

    IRBuilder<> Builder(AddI);

    int numVal = lastInstVec.size();
    while (numVal != 1)
    {
        for (int i=0; i<numVal/2; i++)
        {
            Value *newAdd = Builder.CreateFAdd(lastInstVec[2*i], lastInstVec[2*i+1]);
            generatedI.insert(newAdd);
            lastInstVec[i] = newAdd;
        }
        if (numVal%2)
        {
            lastInstVec[(numVal+1)/2-1] = lastInstVec[numVal-1];
        }
        numVal = (numVal+1)/2;
    }

    auto newAddAll = dyn_cast<Instruction>(lastInstVec[0]);
    assert(newAddAll);

    if (DEBUG) *HI_LoadALAPLog << "new add instruction generated: " << *newAddAll << " to replace " << *AddI << " and the new block is:\n";
    AddI->replaceAllUsesWith(newAddAll);
    RecursivelyDeleteTriviallyDeadInstructions(AddI);
    if (DEBUG) *HI_LoadALAPLog << *newAddAll->getParent() << "\n==================================\n\n";
    return true;        
}



void HI_LoadALAP::recursiveGetFAddOperand(Value *AddI, std::vector<Value*> &nonLoadInsts, std::vector<Value*> &LoadInsts)
{
    if (auto real_AddI = dyn_cast<Instruction>(AddI))
    {
        if (real_AddI->getOpcode() != Instruction::FAdd)
        {
            return;
        }
        for (int i=0; i<real_AddI->getNumOperands(); i++)
        {
            Instruction* opAdd = dyn_cast<Instruction>(real_AddI->getOperand(i));

            bool continueRecursive = false;

            if (opAdd)
            {
                if (opAdd->getOpcode() == Instruction::FAdd)
                    continueRecursive = true;
            }


            if (continueRecursive)
            {
                recursiveGetFAddOperand(opAdd, nonLoadInsts, LoadInsts);          
            }
            else
            {
                if (auto LoadI = dyn_cast<LoadInst>(real_AddI->getOperand(i)))
                {
                    LoadInsts.push_back(LoadI);
                }
                else
                {
                    nonLoadInsts.push_back(real_AddI->getOperand(i));
                }
            }   
        }
    }

}