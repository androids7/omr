/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include "codegen/IA32SystemLinkage.hpp"

#include <stdio.h>                         // for NULL, printf
#include "codegen/CodeGenerator.hpp"       // for CodeGenerator
#include "codegen/IA32LinkageUtils.hpp"    // for IA32LinkageUtils
#include "codegen/Linkage.hpp"             // for TR_X86LinkageProperties, etc
#include "codegen/Machine.hpp"             // for Machine
#include "codegen/RealRegister.hpp"        // for TR::RealRegister::RegNum, etc
#include "codegen/RegisterConstants.hpp"   // for TR_RegisterKinds::TR_X87
#include "codegen/RegisterDependency.hpp"
#include "compile/Compilation.hpp"         // for Compilation
#include "compile/Method.hpp"              // for TR_Method
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "il/ILOpCodes.hpp"                // for ILOpCodes
#include "il/Node.hpp"                     // for Node
#include "il/Node_inlines.hpp"             // for Node::getDataType, etc
#include "il/Symbol.hpp"                   // for Symbol
#include "il/symbol/LabelSymbol.hpp"       // for generateLabelSymbol, etc
#include "il/symbol/MethodSymbol.hpp"      // for MethodSymbol
#include "il/symbol/ParameterSymbol.hpp"   // for ParameterSymbol
#include "infra/Assert.hpp"                // for TR_ASSERT
#include "ras/Debug.hpp"                   // for TR_DebugBase
#include "codegen/X86Instruction.hpp"
#include "x/codegen/X86Ops.hpp"            // for ::ADD4RegImm4, etc

namespace TR { class SymbolReference; }
namespace TR { class MemoryReference; }

////////////////////////////////////////////////
//
// Helpful definitions
//
// These are only here to make the rest of the code below somewhat
// self-documenting.
//
enum
   {
   RETURN_ADDRESS_SIZE=4,
   GPR_REG_WIDTH=4,
   IA32_STACK_SLOT_SIZE=4,
   IA32_DEFAULT_STACK_ALIGNMENT=4
   };

TR_IA32SystemLinkage::TR_IA32SystemLinkage(
      TR::CodeGenerator *cg) :
   TR_X86SystemLinkage(cg)
   {
   _properties._properties = CallerCleanup | AlwaysDedicateFramePointerRegister;
   // for shrinkwrapping, we cannot use pushes for the preserved regs. pushes/pops need to be in sequence and this is not compatible with shrinkwrapping as registers need not be saved/restored in sequenc
   if (cg->comp()->getOption(TR_DisableShrinkWrapping))
      _properties._properties |= UsesPushesForPreservedRegs;

   _properties._registerFlags[TR::RealRegister::NoReg] = 0;
   _properties._registerFlags[TR::RealRegister::eax]   = IntegerReturn;
   _properties._registerFlags[TR::RealRegister::ebx]   = Preserved;
   _properties._registerFlags[TR::RealRegister::ecx]   = 0;
   _properties._registerFlags[TR::RealRegister::edx]   = IntegerReturn;
   _properties._registerFlags[TR::RealRegister::edi]   = Preserved;
   _properties._registerFlags[TR::RealRegister::esi]   = Preserved;
   _properties._registerFlags[TR::RealRegister::ebp]   = Preserved;
   _properties._registerFlags[TR::RealRegister::esp]   = Preserved;
   _properties._registerFlags[TR::RealRegister::st0]   = FloatReturn;

   _properties._preservedRegisters[0] = TR::RealRegister::ebx;
   _properties._preservedRegisters[1] = TR::RealRegister::edi;
   _properties._preservedRegisters[2] = TR::RealRegister::esi;
   _properties._maxRegistersPreservedInPrologue = 3;
   _properties._preservedRegisters[3] = TR::RealRegister::ebp;
   _properties._preservedRegisters[4] = TR::RealRegister::esp;

   _properties._argumentRegisters[0] = TR::RealRegister::NoReg;

   _properties._returnRegisters[0] = TR::RealRegister::eax; // IntegerReturnRegiste
   _properties._returnRegisters[1] = TR::RealRegister::st0; // Float/Double ReturnReg
   _properties._returnRegisters[2] = TR::RealRegister::edx; // LongHighReturnRegister

   _properties._numIntegerArgumentRegisters = 0;
   _properties._firstIntegerArgumentRegister = 0;
   _properties._numFloatArgumentRegisters    = 0;
   _properties._firstFloatArgumentRegister   = 0;

   _properties._framePointerRegister = TR::RealRegister::ebp;
   _properties._methodMetaDataRegister = TR::RealRegister::NoReg;

   _properties._preservedRegisterMapForGC =
      (  (1 << (TR::RealRegister::ebx-1)) |
         (1 << (TR::RealRegister::esi-1)) |
         (1 << (TR::RealRegister::edi-1)) |
         (1 << (TR::RealRegister::ebp-1)) |
         (1 << (TR::RealRegister::esp-1)));

   uint32_t p = 0;
   _properties._volatileRegisters[p++] = TR::RealRegister::eax;
   _properties._volatileRegisters[p++] = TR::RealRegister::ecx;
   _properties._volatileRegisters[p++] = TR::RealRegister::edx;
   _properties._numberOfVolatileGPRegisters = p;

   for (int32_t r=0; r<=7; r++)
      {
      _properties._volatileRegisters[p++] = TR::RealRegister::xmmIndex(r);
      }
   _properties._numberOfVolatileXMMRegisters = p - _properties._numberOfVolatileGPRegisters;
   _properties._numVolatileRegisters = p;

   _properties._offsetToFirstParm = RETURN_ADDRESS_SIZE;
   _properties._offsetToFirstLocal = _properties.getAlwaysDedicateFramePointerRegister() ? -GPR_REG_WIDTH : 0;
   _properties._OutgoingArgAlignment = IA32_DEFAULT_STACK_ALIGNMENT;

   p = 0;
   // Volatiles that aren't linkage regs
   _properties._allocationOrder[p++] = TR::RealRegister::ecx;

   // Linkage regis
   _properties._allocationOrder[p++] = TR::RealRegister::edx;
   _properties._allocationOrder[p++] = TR::RealRegister::eax;

   // preserved register
   _properties._allocationOrder[p++] = TR::RealRegister::esi;
   _properties._allocationOrder[p++] = TR::RealRegister::edi;
   _properties._allocationOrder[p++] = TR::RealRegister::ebx;
   _properties._allocationOrder[p++] = TR::RealRegister::ebp;

   // floating point
   _properties._allocationOrder[p++] = TR::RealRegister::st0;
   _properties._allocationOrder[p++] = TR::RealRegister::st1;
   _properties._allocationOrder[p++] = TR::RealRegister::st2;
   _properties._allocationOrder[p++] = TR::RealRegister::st3;
   _properties._allocationOrder[p++] = TR::RealRegister::st4;
   _properties._allocationOrder[p++] = TR::RealRegister::st5;
   _properties._allocationOrder[p++] = TR::RealRegister::st6;
   _properties._allocationOrder[p++] = TR::RealRegister::st7;

   }

uint32_t
TR_IA32SystemLinkage::getAlignment(TR::DataTypes type)
   {
   switch(type)
      {
      case TR::Float:
         return 4;
      case TR::Double:
         return 4;
      case TR::Int8:
         return 1;
      case TR::Int16:
         return 2;
      case TR::Int32:
         return 4;
      case TR::Address:
      case TR::Int64:
         return 4;
      // TODO: struct/union support should be added as a case branch.
      case TR::Aggregate:
      default:
         TR_ASSERT(false, "types still not support in layout\n");
         return 0;
      }
   }


int32_t
TR_IA32SystemLinkage::layoutParm(TR::Node *parmNode, int32_t &dataCursor, uint16_t &intReg, uint16_t &floatReg, parmLayoutResult &layoutResult)
   {
   layoutResult.abstract |= parmLayoutResult::ON_STACK;
   int32_t align = layoutTypeOnStack(parmNode->getDataType(), dataCursor, layoutResult);
   if (comp()->getOption(TR_TraceCG))
      traceMsg(comp(), "layout param node %p on stack\n", parmNode);
   return align;
   }

int32_t
TR_IA32SystemLinkage::layoutParm(TR::ParameterSymbol *parmSymbol, int32_t &dataCursor, uint16_t &intReg, uint16_t &floatReg, parmLayoutResult &layoutResult)
   {
   layoutResult.abstract |= parmLayoutResult::ON_STACK;
   int32_t align = layoutTypeOnStack(parmSymbol->getDataType(), dataCursor, layoutResult);
   if (comp()->getOption(TR_TraceCG))
      traceMsg(comp(), "layout param symbol %p on stack\n", parmSymbol);
   return align;
   }


void
TR_IA32SystemLinkage::setUpStackSizeForCallNode(TR::Node* node)
   {
   const TR_X86LinkageProperties     &properties = getProperties();
   int32_t  sizeOfOutGoingArgs= 0;
   uint16_t intReg = 0, floatReg = 0;
   for (int32_t i= node->getFirstArgumentIndex(); i<node->getNumChildren(); ++i)
      {
      parmLayoutResult fakeParm;
      TR::Node *parmNode = node->getChild(i);
      layoutParm(parmNode, sizeOfOutGoingArgs, intReg, floatReg, fakeParm);
      }

   if (sizeOfOutGoingArgs > cg()->getLargestOutgoingArgSize())
      {
      cg()->setLargestOutgoingArgSize(sizeOfOutGoingArgs);
      if (comp()->getOption(TR_TraceCG))
         traceMsg(comp(), "setUpStackSizeForCallNode setLargestOutgoingArgSize %d(for call node %p)\n", sizeOfOutGoingArgs, node);
      }

   }

TR::Register *
TR_IA32SystemLinkage::buildIndirectDispatch(TR::Node *callNode)
   {
   TR_ASSERT(false, "TR_IA32SystemLinkage::buildIndirectDispatch is not supported.\n");
   return NULL;
   }

TR::Register *
TR_IA32SystemLinkage::buildVolatileAndReturnDependencies(
    TR::Node *callNode,
    TR::RegisterDependencyConditions *deps)
   {
   TR_ASSERT(deps != NULL, "expected register dependencies");

   // Allocate virtual register for return value
   //
   TR::Register     *integerReturnReg = NULL;
   TR::Register     *fpReturnReg      = NULL;
   TR::Register     *returnReg        = NULL; // An alias for one of the above
   switch (callNode->getDataType())
      {
      case TR::NoType:
         break;
      case TR::Int8:
      case TR::Int16:
      case TR::Int32:
         returnReg = integerReturnReg = cg()->allocateRegister();
         break;
      case TR::Address:
         returnReg = integerReturnReg = cg()->allocateCollectedReferenceRegister();
         break;
      case TR::Float:
         returnReg = fpReturnReg = cg()->allocateSinglePrecisionRegister(TR_X87);
         break;
      case TR::Double:
         returnReg = fpReturnReg = cg()->allocateRegister(TR_X87);
         break;
      case TR::Int64:
      case TR::Aggregate:
      default:
         TR_ASSERT(false, "return type still not supported");
      }

   // Deps for volatile regs
   //

   // TODO: This should be less dependent on the real registers, but the way
   // _properties is set up makes that very hard.
   TR_ASSERT(_properties.getIntegerReturnRegister()  == TR::RealRegister::eax, "assertion failure");
   TR_ASSERT(_properties.getLongLowReturnRegister()  == TR::RealRegister::eax, "assertion failure");
   TR_ASSERT(_properties.getLongHighReturnRegister() == TR::RealRegister::edx, "assertion failure");
   TR_ASSERT(_properties.getFloatReturnRegister()    == TR::RealRegister::st0, "assertion failure");

   if (integerReturnReg)
      deps->addPostCondition(returnReg, TR::RealRegister::eax, cg());
   else
      deps->addPostCondition(cg()->allocateRegister(), TR::RealRegister::eax, cg());

   deps->addPostCondition(cg()->allocateRegister(), TR::RealRegister::ecx, cg());
   deps->addPostCondition(cg()->allocateRegister(), TR::RealRegister::edx, cg());

   // st0
   if (fpReturnReg)
      {
      deps->addPostCondition(returnReg, _properties.getFloatReturnRegister(), cg());
      }
   else
      {
      // No need for a dummy dep here because FPREGSPILL instruction takes care of it
      }

 // The reg dependency is left open intentionally, and need to be closed by
 // the caller. The reason is because, child class might call this method, while
 // adding more register dependecies;  if we close the reg dependency here,
 // the child class could add NO more register dependencies.

   return returnReg;
   }

int32_t TR_IA32SystemLinkage::buildArgs(
      TR::Node *callNode,
      TR::RegisterDependencyConditions *deps)
   {
   // Push args in reverse order for a system call
   //
   int32_t argSize = 0;
   int32_t firstArg = callNode->getFirstArgumentIndex();
   for (int i = callNode->getNumChildren() - 1; i >= firstArg; i--)
      {
      TR::Node *child = callNode->getChild(i);
      switch (child->getDataType())
         {
         case TR::Int8:
         case TR::Int16:
         case TR::Address:
         case TR::Int32:
            TR::IA32LinkageUtils::pushIntegerWordArg(child, cg());
            argSize += 4;
            break;
         case TR::Float:
            TR::IA32LinkageUtils::pushFloatArg(child,cg());
            argSize += 4;
            break;
         case TR::Double:
            TR::IA32LinkageUtils::pushDoubleArg(child, cg());
            argSize += 8;
            break;
         case TR::Aggregate:
         case TR::Int64:
         default:
            TR_ASSERT(0, "Attempted to push unknown type");
            break;
         }
      }
   return argSize;
   }

TR::Register *TR_IA32SystemLinkage::buildDirectDispatch(TR::Node *callNode, bool spillFPRegs)
   {
   TR::RealRegister    *stackPointerReg = machine()->getX86RealRegister(TR::RealRegister::esp);
   TR::SymbolReference *methodSymRef    = callNode->getSymbolReference();
   TR::MethodSymbol    *methodSymbol    = callNode->getSymbol()->castToMethodSymbol();
   TR::ILOpCodes        callOpCodeValue = callNode->getOpCodeValue();

   if (!methodSymbol->isHelper())
      diagnostic("Building call site for %s\n", methodSymbol->getMethod()->signature(trMemory()));

   TR::RegisterDependencyConditions  *deps;
   deps = generateRegisterDependencyConditions((uint8_t)0, (uint8_t)6, cg());
   TR::Register *returnReg = buildVolatileAndReturnDependencies(callNode, deps);
   deps->stopAddingConditions();

   TR::RegisterDependencyConditions  *dummy = generateRegisterDependencyConditions((uint8_t)0, (uint8_t)0, cg());

   uint32_t  argSize = buildArgs(callNode, dummy);

   TR::Register* targetAddressReg = NULL;
   TR::MemoryReference* targetAddressMem = NULL;

   // Call-out
   int32_t stackAdjustment = cg()->getProperties().getCallerCleanup() ? 0 : -argSize;
   TR::X86ImmInstruction* instr = generateImmSymInstruction(CALLImm4, callNode, (uintptr_t)methodSymbol->getMethodAddress(), methodSymRef, cg());
   instr->setAdjustsFramePointerBy(stackAdjustment);

   if (cg()->getProperties().getCallerCleanup() && argSize > 0)
      {
      // Clean up arguments
      //
      generateRegImmInstruction(
         (argSize <= 127) ? ADD4RegImms : ADD4RegImm4,
         callNode,
         stackPointerReg,
         argSize,
         cg()
         );
      }

   // Label denoting end of dispatch code sequence; dependencies are on
   // this label rather than on the call
   //
   TR::LabelSymbol *endSystemCallSequence = generateLabelSymbol(cg());
   generateLabelInstruction(LABEL, callNode, endSystemCallSequence, deps, cg());

   // Stop using the killed registers that are not going to persist
   //
   if (deps)
      stopUsingKilledRegisters(deps, returnReg);

   // If the method returns a floating point value that is not used, insert a dummy store to
   // eventually pop the value from the floating point stack.
   //
   if ((callNode->getDataType() == TR::Float ||
        callNode->getDataType() == TR::Double) &&
       callNode->getReferenceCount() == 1)
      {
      generateFPSTiST0RegRegInstruction(FSTRegReg, callNode, returnReg, returnReg, cg());
      }

   if (cg()->enableRegisterAssociations())
      associatePreservedRegisters(deps, returnReg);

   return returnReg;
   }
