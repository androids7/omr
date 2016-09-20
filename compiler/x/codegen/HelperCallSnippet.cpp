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

#include "x/codegen/HelperCallSnippet.hpp"

#include <stddef.h>                              // for NULL
#include <stdint.h>                              // for int32_t, uint8_t, etc
#include "codegen/CodeGenerator.hpp"             // for CodeGenerator, etc
#include "codegen/FrontEnd.hpp"                  // for TR_FrontEnd
#include "codegen/Instruction.hpp"               // for Instruction
#include "codegen/Linkage.hpp"                   // for Linkage, etc
#include "codegen/Machine.hpp"                   // for Machine
#include "codegen/RealRegister.hpp"              // for RealRegister
#include "codegen/RegisterConstants.hpp"
#include "codegen/RegisterDependency.hpp"
#include "codegen/RegisterDependencyStruct.hpp"  // for RegisterDependency
#include "codegen/Snippet.hpp"                   // for commentString
#include "codegen/SnippetGCMap.hpp"
#include "compile/Compilation.hpp"               // for Compilation, isSMP
#include "compile/SymbolReferenceTable.hpp"
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/jittypes.h"
#include "il/ILOpCodes.hpp"                      // for ILOpCodes::loadaddr
#include "il/ILOps.hpp"                          // for ILOpCode
#include "il/Node.hpp"                           // for Node
#include "il/Node_inlines.hpp"                   // for Node::getChild, etc
#include "il/Symbol.hpp"                         // for Symbol
#include "il/SymbolReference.hpp"                // for SymbolReference
#include "il/symbol/LabelSymbol.hpp"             // for LabelSymbol
#include "il/symbol/MethodSymbol.hpp"            // for MethodSymbol
#include "il/symbol/ResolvedMethodSymbol.hpp"
#include "il/symbol/StaticSymbol.hpp"            // for StaticSymbol
#include "infra/Assert.hpp"                      // for TR_ASSERT
#include "ras/Debug.hpp"                         // for TR_Debug
#include "runtime/Runtime.hpp"                   // for ::TR_HelperAddress
#include "x/codegen/RestartSnippet.hpp"
#include "env/IO.hpp"
#include "env/CompilerEnv.hpp"

TR_X86HelperCallSnippet::TR_X86HelperCallSnippet(TR::CodeGenerator   *cg,
                                                 TR::Node            *node,
                                                 TR::LabelSymbol      *restartlab,
                                                 TR::LabelSymbol      *snippetlab,
                                                 TR::SymbolReference *helper,
                                                 int32_t             stackPointerAdjustment)
   : TR_X86RestartSnippet(cg, node, restartlab, snippetlab, helper->canCauseGC()),
     _destination(helper),
     _callNode(0),
     _offset(-1),
     _callInstructionBufferAddress(NULL),
     _stackPointerAdjustment(stackPointerAdjustment),
     _alignCallDisplacementForPatching(false)
   {
   // The jitReportMethodEnter helper requires special handling; the first
   // child of the call node is the receiver object of the current (owning)
   // method, which is not a constant value. We pass the receiver object to
   // the helper by pushing the first argument to the current method.
   TR::Compilation* comp = cg->comp();
   TR::SymbolReferenceTable *symRefTab = comp->getSymRefTab();
   TR::ResolvedMethodSymbol    *owningMethod = comp->getJittedMethodSymbol();

   if (getDestination() == symRefTab->findOrCreateReportMethodEnterSymbolRef(owningMethod))
      {
      _offset = ((int32_t) owningMethod->getNumParameterSlots() * 4);
      }
   }

TR_X86HelperCallSnippet::TR_X86HelperCallSnippet(TR::CodeGenerator *cg,
                                                 TR::LabelSymbol    *restartlab,
                                                 TR::LabelSymbol    *snippetlab,
                                                 TR::Node          *callNode,
                                                 int32_t           stackPointerAdjustment)
   : TR_X86RestartSnippet(cg, callNode, restartlab, snippetlab, callNode->getSymbolReference()->canCauseGC()),
     _destination(callNode->getSymbolReference()),
     _callNode(callNode),
     _offset(-1),
     _callInstructionBufferAddress(NULL),
     _stackPointerAdjustment(stackPointerAdjustment),
     _alignCallDisplacementForPatching(false)
   {
   TR::Compilation* comp = cg->comp();
   TR::SymbolReferenceTable *symRefTab    = comp->getSymRefTab();
   TR::ResolvedMethodSymbol    *owningMethod = comp->getJittedMethodSymbol();

   if (getDestination() == symRefTab->findOrCreateReportMethodEnterSymbolRef(owningMethod))
      {
      _offset = ((int32_t) owningMethod->getNumParameterSlots() * 4);
      }
   }

uint8_t *TR_X86HelperCallSnippet::emitSnippetBody()
   {
   uint8_t *buffer = cg()->getBinaryBufferCursor();
   getSnippetLabel()->setCodeLocation(buffer);

   uint8_t * grm = genRestartJump(genHelperCall(buffer));

   return grm;
   }


void
TR_X86HelperCallSnippet::addMetaDataForLoadAddrArg(
      uint8_t *buffer,
      TR::Node *child)
   {
   TR::StaticSymbol *sym = child->getSymbol()->getStaticSymbol();

   if (cg()->comp()->getOption(TR_EnableHCR)
       && (!child->getSymbol()->isClassObject()
           || cg()->wantToPatchClassPointer((TR_OpaqueClassBlock*)sym->getStaticAddress(), buffer)))
      {
      if (TR::Compiler->target.is64Bit())
         cg()->jitAddPicToPatchOnClassRedefinition(((void *) (uintptrj_t)sym->getStaticAddress()), (void *) buffer);
      else
         cg()->jitAdd32BitPicToPatchOnClassRedefinition(((void *) (uintptrj_t)sym->getStaticAddress()), (void *) buffer);
      }

   }


uint8_t *TR_X86HelperCallSnippet::genHelperCall(uint8_t *buffer)
   {
   // add esp, _stackPointerAdjustment
   //
   if (_stackPointerAdjustment < -128 || _stackPointerAdjustment > 127)
      {
      if (TR::Compiler->target.is64Bit())
         {
         *buffer++ = 0x48; // Rex
         }
      *buffer++ = 0x81;
      *buffer++ = 0xc4;
      *(int32_t*)buffer = _stackPointerAdjustment;
      buffer += 4;
      }
   else if (_stackPointerAdjustment != 0)
      {
      if (TR::Compiler->target.is64Bit())
         {
         *buffer++ = 0x48; // Rex
         }
      *buffer++ = 0x83;
      *buffer++ = 0xc4;
      *buffer++ = (uint8_t)_stackPointerAdjustment;
      }

   if (_callNode)
      {
      if(!debug("amd64unimplemented"))
         TR_ASSERT(TR::Compiler->target.is32Bit(), "AMD64 genHelperCall with _callNode not yet implemented");
      int32_t i = 0;

      if (_offset != -1)
         {
         // push [vfp +N]  ; N is the offset from vfp to arg 0
         //
         *buffer++ = 0xFF;
         if (cg()->getLinkage()->getProperties().getAlwaysDedicateFramePointerRegister())
            {
            if (_offset > -128 && _offset <= 127)
               {
               *buffer++ = 0x73;
               *(int8_t *)buffer++ = _offset;
               }
            else
               {
               *buffer++ = 0xB3;
               *(int32_t *)buffer = _offset;
               buffer += 4;
               }
            }
         else
            {
            _offset += cg()->getFrameSizeInBytes();
            if (_offset > -128 && _offset <= 127)
               {
               *buffer++ = 0x74;
               *buffer++ = 0x24;
               *(int8_t *)buffer++ = _offset;
               }
            else
               {
               *buffer++ = 0xB4;
               *buffer++ = 0x24;
               *(int32_t *)buffer = _offset;
               buffer += 4;
               }
            }
         i = 1; // skip the first child
         }

      TR::RegisterDependencyConditions  *deps = getRestartLabel()->getInstruction()->getDependencyConditions();
      int32_t registerArgs = 0;

      for ( ; i < _callNode->getNumChildren(); ++i)
         {
         TR::Node *child = _callNode->getChild(i);

         if (child->getOpCodeValue() == TR::loadaddr)
            {
            if (!child->getRegister() ||
                child->getRegister() != deps->getPostConditions()->getRegisterDependency(registerArgs)->getRegister())
               {
               TR::StaticSymbol *sym = child->getSymbol()->getStaticSymbol();
               TR_ASSERT(sym, "Bad argument to helper call");
               *buffer++ = 0x68; // push   imm4   argValue
               *(uint32_t *)buffer = (uint32_t)(uintptrj_t)sym->getStaticAddress();

               addMetaDataForLoadAddrArg(buffer, child);

               buffer += 4;
               continue;
               }
            }
         else if (child->getOpCode().isLoadConst())
            {
            int32_t argValue = child->getInt();
            if (argValue >= -128 && argValue <= 127)
               {
               *buffer++ = 0x6a; // push   imms   argValue
               *(int8_t *)buffer = argValue;
               buffer += 1;
               }
            else
               {
               *buffer++ = 0x68; // push   imm4   argValue
               *(int32_t *)buffer = argValue;
               buffer += 4;
               }
            continue;
            }

         // Find out the register from the dependency list on the restart
         // label instruction
         //
         TR_ASSERT(child->getRegister(), "Bad argument to helper call");

         *buffer = 0x50;
         TR_ASSERT(deps, "null dependencies on restart label of helper call snippet with register args");
         cg()->machine()->getX86RealRegister(deps->getPostConditions()->getRegisterDependency(registerArgs++)->getRealRegister())->setRegisterFieldInOpcode(buffer++);
         }
      }


   // Insert alignment padding if the instruction might be patched dynamically.
   //
   if (_alignCallDisplacementForPatching && TR::Compiler->target.isSMP())
      {
      uintptrj_t mod = (uintptrj_t)(buffer) % cg()->getInstructionPatchAlignmentBoundary();
      mod = cg()->getInstructionPatchAlignmentBoundary() - mod;

      if (mod <= 4)
         {
         // Perhaps use a multi-byte NOP here...
         //
         while (mod--)
            {
            *buffer++ = 0x90;
            }
         }
      }

   _callInstructionBufferAddress = buffer;

   *buffer++ = 0xe8; // CallImm4
   *(int32_t *)buffer = branchDisplacementToHelper(buffer+4, getDestination(), cg());

   cg()->addProjectSpecializedRelocation(buffer,(uint8_t *)getDestination(), NULL, TR_HelperAddress, __FILE__, __LINE__, _callNode);

   buffer += 4;

   gcMap().registerStackMap(buffer, cg());

   // sub esp, _stackPointerAdjustment
   //
   if (_stackPointerAdjustment < -128 || _stackPointerAdjustment > 127)
      {
      if (TR::Compiler->target.is64Bit())
         {
         *buffer++ = 0x48; // Rex
         }
      *buffer++ = 0x81;
      *buffer++ = 0xec;
      *(int32_t*)buffer = _stackPointerAdjustment;
      buffer += 4;
      }
   else if (_stackPointerAdjustment != 0)
      {
      if (TR::Compiler->target.is64Bit())
         {
         *buffer++ = 0x48; // Rex
         }
      *buffer++ = 0x83;
      *buffer++ = 0xec;
      *buffer++ = (uint8_t)_stackPointerAdjustment;
      }

   return buffer;
   }


void
TR_Debug::print(TR::FILE *pOutFile, TR_X86HelperCallSnippet  * snippet)
   {
   // *this    swipeable for debugging purposes
   if (pOutFile == NULL)
      return;
   uint8_t *bufferPos = snippet->getSnippetLabel()->getCodeLocation();
   printSnippetLabel(pOutFile, snippet->getSnippetLabel(), bufferPos, getName(snippet), getName(snippet->getDestination()));
   printBody(pOutFile, snippet, bufferPos);
   }

void
TR_Debug::printBody(TR::FILE *pOutFile, TR_X86HelperCallSnippet  * snippet, uint8_t *bufferPos)
   {
   // *this    swipeable for debugging purposes
   TR_ASSERT(pOutFile != NULL, "assertion failure");
   TR::MethodSymbol *sym = snippet->getDestination()->getSymbol()->castToMethodSymbol();

   int32_t i = 0;

   if (snippet->getStackPointerAdjustment() != 0)
      {
      uint8_t size = 5 + (TR::Compiler->target.is64Bit()? 1 : 0);
      printPrefix(pOutFile, NULL, bufferPos, size);
      trfprintf(pOutFile, "add \t%s, %d\t\t\t%s Temporarily deallocate stack frame", TR::Compiler->target.is64Bit()? "rsp":"esp", snippet->getStackPointerAdjustment(),
                    commentString());
      bufferPos += size;
      }

   if (snippet->getCallNode())
      {
      if (snippet->getOffset() != -1)
         {
         uint32_t pushLength;

         bool useDedicatedFrameReg = _comp->cg()->getLinkage()->getProperties().getAlwaysDedicateFramePointerRegister();
         if (snippet->getOffset() >= -128 && snippet->getOffset() <= 127)
            pushLength = (useDedicatedFrameReg ? 3 : 4);
         else
            pushLength = (useDedicatedFrameReg ? 6 : 7);

         printPrefix(pOutFile, NULL, bufferPos, pushLength);
         trfprintf(pOutFile,
                       "push\t[%s +%d]\t%s Address of Receiver",
                       useDedicatedFrameReg ? "ebx" : "esp",
                       snippet->getOffset(),
                       commentString());

         bufferPos += pushLength;
         i = 1; // skip the first child
         }

      TR::RegisterDependencyConditions  *deps = snippet->getRestartLabel()->getInstruction()->getDependencyConditions();
      int32_t registerArgs = 0;
      for ( ; i < snippet->getCallNode()->getNumChildren(); i++)
         {
         TR::Node *child = snippet->getCallNode()->getChild(i);
         if (child->getOpCodeValue() == TR::loadaddr && !child->getRegister())
            {
            TR::StaticSymbol *sym = child->getSymbol()->getStaticSymbol();
            TR_ASSERT( sym, "Bad argument to helper call");
            printPrefix(pOutFile, NULL, bufferPos, 5);
            trfprintf(pOutFile, "push\t" POINTER_PRINTF_FORMAT, sym->getStaticAddress());
            bufferPos += 5;
            }
         else if (child->getOpCode().isLoadConst())
            {
            int32_t argValue = child->getInt();
            int32_t size = (argValue >= -128 && argValue <= 127) ? 2 : 5;
            printPrefix(pOutFile, NULL, bufferPos, size);
            trfprintf(pOutFile, "push\t" POINTER_PRINTF_FORMAT, argValue);
            bufferPos += size;
            }
         else
            {
            TR_ASSERT( child->getRegister(), "Bad argument to helper call");

            printPrefix(pOutFile, NULL, bufferPos, 1);
            trfprintf(pOutFile, "push\t");
            TR_ASSERT( deps, "null dependencies on restart label of helper call snippet with register args");
            print(pOutFile, _cg->machine()->getX86RealRegister(deps->getPostConditions()->getRegisterDependency(registerArgs++)->getRealRegister()), TR_WordReg);
            bufferPos++;
            }
         }
      }

   printPrefix(pOutFile, NULL, bufferPos, 5);
   trfprintf(pOutFile, "call\t%s \t%s Helper Address = " POINTER_PRINTF_FORMAT,
                 getName(snippet->getDestination()),
                 commentString(),
                 sym->getMethodAddress());
   bufferPos += 5;

   if (snippet->getStackPointerAdjustment() != 0)
      {
      uint8_t size = 5 + (TR::Compiler->target.is64Bit()? 1 : 0);
      printPrefix(pOutFile, NULL, bufferPos, size);
      trfprintf(pOutFile, "sub \t%s, %d\t\t\t%s Reallocate stack frame", TR::Compiler->target.is64Bit()? "rsp":"esp", snippet->getStackPointerAdjustment(),
                    commentString());
      bufferPos += size;
      }

   printRestartJump(pOutFile, snippet, bufferPos);
   }


uint32_t TR_X86HelperCallSnippet::getLength(int32_t estimatedSnippetStart)
   {
   // *this    swipeable for debugging purposes
   uint32_t length = 35;

   if (_callNode)
      {
      int32_t i = 0;

      if (_offset != -1)
         {
         bool useDedicatedFrameReg = cg()->getLinkage()->getProperties().getAlwaysDedicateFramePointerRegister();
         if (_offset >= -128 && _offset <= 127)
            length += (useDedicatedFrameReg ? 3 : 4);
         else
            length += (useDedicatedFrameReg ? 6 : 7);

         i = 1; // skip the first child
         }

      TR::RegisterDependencyConditions  *deps = getRestartLabel()->getInstruction()->getDependencyConditions();
      int32_t registerArgs = 0;

      for ( ; i < _callNode->getNumChildren(); ++i)
         {
         TR::Node *child = _callNode->getChild(i);
         if (child->getOpCodeValue() == TR::loadaddr &&
            (!child->getRegister() ||
                child->getRegister() != deps->getPostConditions()->getRegisterDependency(registerArgs++)->getRegister()))
            {
            length += 5;
            }
         else if (child->getOpCode().isLoadConst())
            {
            int32_t argValue = child->getInt();
            if (argValue >= -128 && argValue <= 127)
               length += 2;
            else
               length += 5;
            }
         else
            {
            length += 1;
            }
         }
      }

   // Conservatively assume that 4 NOPs might be required for alignment.
   //
   if (_alignCallDisplacementForPatching && TR::Compiler->target.isSMP())
      {
      length += 4;
      }

   return length + estimateRestartJumpLength(estimatedSnippetStart + length);
   }


int32_t TR_X86HelperCallSnippet::branchDisplacementToHelper(
   uint8_t            *nextInstructionAddress,
   TR::SymbolReference *helper,
   TR::CodeGenerator   *cg)
   {
   intptrj_t helperAddress = (intptrj_t)helper->getMethodAddress();

   if (NEEDS_TRAMPOLINE(helperAddress, nextInstructionAddress, cg))
      {
      helperAddress = cg->fe()->indexedTrampolineLookup(helper->getReferenceNumber(), (void *)(nextInstructionAddress-4));
      TR_ASSERT(IS_32BIT_RIP(helperAddress, nextInstructionAddress), "Local helper trampoline should be reachable directly.\n");
      }

   return (int32_t)(helperAddress - (intptrj_t)(nextInstructionAddress));
   }


uint8_t *TR_X86CheckAsyncMessagesSnippet::genHelperCall(uint8_t *buffer)
   {
   // This code assumes that the superclass' genHelperCall() will only emit
   // the call and nothing more.  This should always be true for the asynccheck
   // helpers.
   //
   uint8_t *cursor = TR_X86HelperCallSnippet::genHelperCall(buffer);
   return cursor;
   }