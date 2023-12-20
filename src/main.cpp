#include <fstream>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unordered_map>
#include <set>

#include "llvm-c/Core.h"
#include "llvm/Analysis/InlineCost.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/CallGraph.h"
//#include "llvm/Analysis/AnalysisManager.h"

#include "llvm/IR/LLVMContext.h"

#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>

using namespace llvm;

static void DoInlining(Module *);

static void summarize(Module *M);

static void print_csv_file(std::string outputfile);

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::Required, cl::init("-"));

static cl::opt<std::string>
        OutputFilename(cl::Positional, cl::desc("<output bitcode>"), cl::Required, cl::init("out.bc"));

static cl::opt<bool>
        InlineHeuristic("inline-heuristic",
              cl::desc("Use student's inlining heuristic."),
              cl::init(false));

static cl::opt<bool>
        InlineConstArg("inline-require-const-arg",
              cl::desc("Require function call to have at least one constant argument."),
              cl::init(false));

static cl::opt<int>
        InlineFunctionSizeLimit("inline-function-size-limit",
              cl::desc("Biggest size of function to inline."),
              cl::init(1000000000));

static cl::opt<int>
        InlineGrowthFactor("inline-growth-factor",
              cl::desc("Largest allowed program size increase factor (e.g. 2x)."),
              cl::init(10));


static cl::opt<bool>
        NoInline("no-inline",
              cl::desc("Do not perform inlining."),
              cl::init(false));


static cl::opt<bool>
        NoPreOpt("no-preopt",
              cl::desc("Do not perform pre-inlining optimizations."),
              cl::init(false));

static cl::opt<bool>
        NoPostOpt("no-postopt",
              cl::desc("Do not perform post-inlining optimizations."),
              cl::init(false));

static cl::opt<bool>
        Verbose("verbose",
                    cl::desc("Verbose stats."),
                    cl::init(false));

static cl::opt<bool>
        NoCheck("no",
                cl::desc("Do not check for valid IR."),
                cl::init(false));


static llvm::Statistic nInstrBeforeOpt = {"", "nInstrBeforeOpt", "number of instructions"};
static llvm::Statistic nInstrBeforeInline = {"", "nInstrPreInline", "number of instructions"};
static llvm::Statistic nInstrAfterInline = {"", "nInstrAfterInline", "number of instructions"};
static llvm::Statistic nInstrPostOpt = {"", "nInstrPostOpt", "number of instructions"};

#define PRINT 0

static void countInstructions(Module *M, llvm::Statistic &nInstr) {
  for (auto i = M->begin(); i != M->end(); i++) {
    for (auto j = i->begin(); j != i->end(); j++) {
      for (auto k = j->begin(); k != j->end(); k++) {
	nInstr++;
      }
    }
  }
}


int main(int argc, char **argv) {
    // Parse command line arguments
    cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

    // Handle creating output files and shutting down properly
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.
    LLVMContext Context;

    // LLVM idiom for constructing output file.
    std::unique_ptr<ToolOutputFile> Out;
    std::string ErrorInfo;
    std::error_code EC;
    Out.reset(new ToolOutputFile(OutputFilename.c_str(), EC,
                                 sys::fs::OF_None));

    EnableStatistics();

    // Read in module
    SMDiagnostic Err;
    std::unique_ptr<Module> M;
    M = parseIRFile(InputFilename, Err, Context);

    // If errors, fail
    if (M.get() == 0)
    {
        Err.print(argv[0], errs());
        return 1;
    }

    countInstructions(M.get(),nInstrBeforeOpt);
    
    if (!NoPreOpt) {
      legacy::PassManager Passes;
      Passes.add(createPromoteMemoryToRegisterPass());    
      Passes.add(createEarlyCSEPass());
      Passes.add(createSCCPPass());
      Passes.add(createAggressiveDCEPass());
      Passes.add(createVerifierPass());
      Passes.run(*M);  
    }

    countInstructions(M.get(),nInstrBeforeInline);    

    if (!NoInline) {
        DoInlining(M.get());
    }

    countInstructions(M.get(),nInstrAfterInline);
    
    if (!NoPostOpt) {
      legacy::PassManager Passes;
      Passes.add(createPromoteMemoryToRegisterPass());    
      Passes.add(createEarlyCSEPass());
      Passes.add(createSCCPPass());
      Passes.add(createAggressiveDCEPass());
      Passes.add(createVerifierPass());
      Passes.run(*M);  
    }

    countInstructions(M.get(),nInstrPostOpt);
    
    // Collect statistics on Module
    summarize(M.get());
    print_csv_file(OutputFilename);

    if (Verbose)
        PrintStatistics(errs());

    // Verify integrity of Module, do this by default
    if (!NoCheck)
    {
        legacy::PassManager Passes;
        Passes.add(createVerifierPass());
        Passes.run(*M.get());
    }

    // Write final bitcode
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();

    return 0;
}

static llvm::Statistic nFunctions = {"", "Functions", "number of functions"};
static llvm::Statistic nInstructions = {"", "Instructions", "number of instructions"};
static llvm::Statistic nLoads = {"", "Loads", "number of loads"};
static llvm::Statistic nStores = {"", "Stores", "number of stores"};

static void summarize(Module *M) {
    for (auto i = M->begin(); i != M->end(); i++) {
        if (i->begin() != i->end()) {
            nFunctions++;
        }

        for (auto j = i->begin(); j != i->end(); j++) {
            for (auto k = j->begin(); k != j->end(); k++) {
                Instruction &I = *k;
                nInstructions++;
                if (isa<LoadInst>(&I)) {
                    nLoads++;
                } else if (isa<StoreInst>(&I)) {
                    nStores++;
                }
            }
        }
    }
}

static void print_csv_file(std::string outputfile)
{
    std::ofstream stats(outputfile + ".stats");
    auto a = GetStatistics();
    for (auto p : a) {
        stats << p.first.str() << "," << p.second << std::endl;
    }
    stats.close();
}

static llvm::Statistic Inlined = {"", "Inlined", "Inlined a call."};
static llvm::Statistic ConstArg = {"", "ConstArg", "Call has a constant argument."};
static llvm::Statistic SizeReq = {"", "SizeReq", "Call has a constant argument."};


#include "llvm/Transforms/Utils/Cloning.h"

unsigned int getNumInstructionsInCalledFunction(CallInst* callInst);
bool getInstructionArgIsConst(CallInst* callInst);

// Use standard template library for a set
std::set<Instruction*> worklist;

static void DoInlining(Module *M) 
{
	// Initially all statictics setting to 0
	Inlined = 0;
	ConstArg = 0;
	SizeReq = 0;

	// Implement a function to perform function inlining
	for(auto f = M->begin(); f!=M->end(); f++)
	{
		// loop over functions
		for(auto bb= f->begin(); bb!=f->end(); bb++)
		{
			// loop over basic blocks
			for(auto i = bb->begin(); i != bb->end(); i++)
			{
				//loop over instructions
				if (isa<CallInst>(&*i))
				{
					worklist.insert(&*i); 
				}
			}    
		}
	}

	// First we are cheking user passed command line input as a InlineHeuristic if not then go in else part
	if(InlineHeuristic) 
	{
		// Enable inlining heuristic
		// -inline-heuristic
		InlineFunctionSizeLimit = 175000;
		InlineGrowthFactor = 9;

				/* Cheking worlist is not empty */
		if(worklist.size()>0)
		{
			// nOrignalCountInline is a unsigned interger to get number of line of orignal code , iTempPrevInstCountNum to store prev count after inlininig Function
			// fNewGrowthFactor is a varriable to store growth factor every time
			unsigned int nOrignalCountInline = nInstrBeforeInline;
			unsigned int iTempPrevInstCountNum = 0,iCountAfterInlining = 0;
			float fNewGrowthFactor = 0;

			/* Rotate worlist till empty */
			while(worklist.size()) 
			{
				// Get the first item 
				Instruction *i = *(worklist.begin());

				// Getting numbers of instruction in called function and storing in iActualNumOfInst
				unsigned int iActualNumOfInst= getNumInstructionsInCalledFunction(dyn_cast<CallInst>(i));

				// Cheking iActualNumOfInst is not more and equal than what user pass or default value of InlineFunctionSizeLimit through command line
				if(InlineFunctionSizeLimit >= iActualNumOfInst  && iActualNumOfInst != 0)
				{
					SizeReq++;

					iCountAfterInlining = nInstrBeforeInline + iActualNumOfInst + iTempPrevInstCountNum;
					iTempPrevInstCountNum = iCountAfterInlining;
					fNewGrowthFactor = iCountAfterInlining / nOrignalCountInline;

					// Cheking fNewGrowthFactor is not more and equal than what user pass or default value of InlineGrowthFactor through command line
					if(InlineGrowthFactor >= fNewGrowthFactor)
					{
						  	// This condition checks if the current instruction is a call instruction using dyn_cast //
							if (CallInst *CI = dyn_cast<CallInst>(i)) 
							{
								// it retrieves the function being called using getCalledFunction and checking it is a function call only
								llvm::Function *F = CI->getCalledFunction();
								if (F) 
								{
									// Create an InlineFunctionInfo object to store information about the inlining process.
									// Check if it is viable to inline the function F by calling the isInlineViable function
									llvm::InlineFunctionInfo IFI;
									llvm::InlineResult IR = isInlineViable(*F);

									// If it is true then Inline this function
									if (IR.isSuccess()) 
									{
										InlineFunction(*CI, IFI);
										Inlined++;		
									}
								}
							}
					}
				}
				// ersaing the instruction from worklist
				worklist.erase(worklist.begin());
			}
		}
		else
		{
			errs()<<"Worklist is empty";
		}
		
	}
	else 
	{
		// When Inline Herustic flag is not specified, then apply other Basic Inlining support. 
		// 1.-inline-function-size-limit=N
		// 2. -inline-growth-factor=M
		// 3. -inline-require-const-arg

		/* Cheking worlist is not empty */
		if(worklist.size()>0)
		{
			// nOrignalCountInline is a unsigned interger to get number of line of orignal code , iTempPrevInstCountNum to store prev count after inlininig Function
			// fNewGrowthFactor is a varriable to store growth factor every time
			unsigned int nOrignalCountInline = nInstrBeforeInline;
			unsigned int iTempPrevInstCountNum = 0,iCountAfterInlining = 0;
			float fNewGrowthFactor = 0;

			/* Rotate worlist till empty */
			while(worklist.size()) 
			{
				// Get the first item 
				Instruction *i = *(worklist.begin());

				// Getting numbers of instruction in called function and storing in iActualNumOfInst
				unsigned int iActualNumOfInst= getNumInstructionsInCalledFunction(dyn_cast<CallInst>(i));

				// Cheking iActualNumOfInst is not more and equal than what user pass or default value of InlineFunctionSizeLimit through command line
				if(InlineFunctionSizeLimit >= iActualNumOfInst  && iActualNumOfInst != 0)
				{
					SizeReq++;

					iCountAfterInlining = nInstrBeforeInline + iActualNumOfInst + iTempPrevInstCountNum;
					iTempPrevInstCountNum = iCountAfterInlining;
					fNewGrowthFactor = iCountAfterInlining / nOrignalCountInline;

					// Those below line under macro use for debug purpose
#if PRINT
					errs() << "nInstrBeforeInline = " <<  iActualNumOfInst << "iCountAfterInlining";
					errs() <<"iCountAfterInlining = " << iCountAfterInlining << "iTempPrevInstCountNum = "<< iTempPrevInstCountNum;
					errs() << "fNewGrowthFactor = "<<fNewGrowthFactor <<"nOrignalCountInline = "<< nOrignalCountInline;
#endif

					// Cheking fNewGrowthFactor is not more and equal than what user pass or default value of InlineGrowthFactor through command line
					if(InlineGrowthFactor >= fNewGrowthFactor)
					{
						/* Cheking if InlineConstArg is default value is  false so we can directly inline all method
						 else InlineConstArg is true and one of the argument in called function is constant then inline the  function 
						*/
						 if(!InlineConstArg)
						{
						  	// This condition checks if the current instruction is a call instruction using dyn_cast //
							if (CallInst *CI = dyn_cast<CallInst>(i)) 
							{
								// it retrieves the function being called using getCalledFunction and checking it is a function call only
								llvm::Function *F = CI->getCalledFunction();
								if (F) 
								{
									// Create an InlineFunctionInfo object to store information about the inlining process.
									// Check if it is viable to inline the function F by calling the isInlineViable function
									llvm::InlineFunctionInfo IFI;
									llvm::InlineResult IR = isInlineViable(*F);

									// If it is true then Inline this function
									if (IR.isSuccess()) 
									{
										InlineFunction(*CI, IFI);
										Inlined++;		
									}

								}
							}
						}
						else if(InlineConstArg && getInstructionArgIsConst(dyn_cast<CallInst>(i)))
						{
							ConstArg++;

							// This condition checks if the current instruction is a call instruction using dyn_cast //
							if (CallInst *CI = dyn_cast<CallInst>(i)) 
							{
								// it retrieves the function being called using getCalledFunction and checking it is a function call only
								llvm::Function *F = CI->getCalledFunction();
								if (F) 
								{
									// Create an InlineFunctionInfo object to store information about the inlining process.
									// Check if it is viable to inline the function F by calling the isInlineViable function
									llvm::InlineFunctionInfo IFI;
									llvm::InlineResult IR = isInlineViable(*F);

									// If it is true then Inline this function
									if (IR.isSuccess()) 
									{
										InlineFunction(*CI, IFI);
										Inlined++;		
									}

								}
							}
						}
						else
						{
						}
					}
				}
				// ersaing the instruction from worklist
				worklist.erase(worklist.begin());
			}
		}
		else
		{
			errs()<<"Worklist is empty";
		}
	}	
}

/* This method is use to find number of instruction of callee function and return the integer value in terms of number of line in called function
   @return : unsigned int
   @arg1 : CallInst
   */
unsigned int getNumInstructionsInCalledFunction(CallInst* callInst) 
{
	Function* calledFunc = callInst->getCalledFunction();
	if (calledFunc == nullptr) 
	{
		return 0;
	}

	unsigned int numInstrs = 0;
	for (const BasicBlock& bb : calledFunc->getBasicBlockList()) 
	{
		numInstrs += bb.getInstList().size();
	}

	return numInstrs;
}

/* This function is checking in called function any arument is constant or not if any argument is constatnt than return true else return false
   @return : bool
   @arg1 : CallInst
   */
bool getInstructionArgIsConst(CallInst* callInst)
{
	// Taking flag to check any argument is constant but defaiult valueI kept false
	bool bContArg = false;

	// Get the function being called
	Function *calledFunc = callInst->getCalledFunction();

	if(calledFunc)
	{
		// Get the number of arguments passed to the function
		unsigned int numArgs = calledFunc->arg_size();

		// Iterate over the arguments and check if any of them are constant
		for (unsigned int i = 0; i < numArgs; i++) 
		{
			// geeting oprand and checking it is constant or not and if it is constant then bContArg set true
			Value *arg = callInst->getArgOperand(i);
			if (Constant *c = dyn_cast<Constant>(arg)) 
			{
				// The argument is a constant
				bContArg = true;
			}
		}
	}
	else
	{
		errs()<<"Called Fun is null";
	}

	return bContArg;
}
