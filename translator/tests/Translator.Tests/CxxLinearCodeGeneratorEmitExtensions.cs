using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Representation;

namespace Translator.Tests;

/// <summary>
/// Test-only wrapper around <see cref="CxxLinearCodeGenerator.EmitWithFacts"/> that returns just the
/// emitted C++ text, so codegen tests don't have to thread an unused facts object through every call.
/// </summary>
internal static class CxxLinearCodeGeneratorEmitExtensions
{
    public static string Emit(
        this CxxLinearCodeGenerator generator,
        uint entryPoint,
        SsaResult ssa,
        FunctionAbiClassification signature,
        RepresentationEnvironment types,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        bool emitStateFreeLeafVariant = false,
        IReadOnlyDictionary<uint, GuestAbiContract>? stateFreeAbiContracts = null,
        IReadOnlyDictionary<uint, string>? stateFreeCallSymbols = null,
        IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>? stateFreeCallSiteVariants = null,
        IReadOnlyList<GuestStateFreeCallVariant>? stateFreeEntryVariants = null,
        IReadOnlyDictionary<string, uint>? gqrEntryConstants = null,
        IReadOnlyDictionary<uint, byte>? gqrCalleeWriteMasks = null,
        bool gqrConstantsRequireRuntimeGuard = false,
        bool enableLeafAbiSpillElision = false,
        bool enableGpuFifoBurstCoalescing = true) =>
        generator.EmitWithFacts(
            entryPoint: entryPoint,
            ssa: ssa,
            signature: signature,
            types: types,
            guestAbiContracts: guestAbiContracts,
            emitStateFreeLeafVariant: emitStateFreeLeafVariant,
            stateFreeAbiContracts: stateFreeAbiContracts,
            stateFreeCallSymbols: stateFreeCallSymbols,
            stateFreeCallSiteVariants: stateFreeCallSiteVariants,
            stateFreeEntryVariants: stateFreeEntryVariants,
            gqrEntryConstants: gqrEntryConstants,
            gqrCalleeWriteMasks: gqrCalleeWriteMasks,
            gqrConstantsRequireRuntimeGuard: gqrConstantsRequireRuntimeGuard,
            enableLeafAbiSpillElision: enableLeafAbiSpillElision,
            enableGpuFifoBurstCoalescing: enableGpuFifoBurstCoalescing).Code;
}
