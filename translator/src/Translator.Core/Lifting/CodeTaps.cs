using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Lifting;

/// <summary>
/// Code taps: instruction sites where the runtime may change one register's value while the game runs, the way a
/// mod that rewrites those instructions on a Wii would (a Gecko code writing an instruction, a replaced game.rel).
/// Each tap lowers to <c>reg = OTHERM_TAP(id, reg)</c>, which is the value itself unless the runtime switched tap
/// <c>id</c> on. "before" taps a register the instruction reads, "after" one it writes, so a tap never changes which
/// registers a function reads or writes and every ABI and liveness fact about the original code still holds.
/// The list (one "&lt;address&gt; before|after &lt;rN&gt;" per line, '#' comments) is project.translation.code_taps; a tap's id
/// is its line's position among the taps, and RuntimeConfig.h lists them for the runtime in the same order.
/// </summary>
public sealed record CodeTap(int Id, uint Address, bool After, string Register);

public static class CodeTaps
{
    public const string TapOp = "tap";

    private static IReadOnlyDictionary<uint, CodeTap[]> s_byAddress = new Dictionary<uint, CodeTap[]>();

    public static IReadOnlyList<CodeTap> All { get; private set; } = Array.Empty<CodeTap>();

    /// <summary>Publishes the project's taps for every lifter in this process (set once, before translation).</summary>
    public static void Publish(IReadOnlyList<CodeTap> taps)
    {
        All = taps;
        s_byAddress = taps.GroupBy(static tap => tap.Address).ToDictionary(static g => g.Key, static g => g.ToArray());
    }

    public static IReadOnlyList<CodeTap> Load(string path)
    {
        var taps = new List<CodeTap>();
        var lineNumber = 0;
        foreach (var raw in File.ReadAllLines(path))
        {
            ++lineNumber;
            var line = raw;
            var comment = line.IndexOf('#');
            if (comment >= 0) line = line[..comment];
            var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) continue;
            if (parts.Length != 3)
                throw new InvalidDataException($"{path}:{lineNumber}: expected '<address> before|after <rN>'");
            var addressText = parts[0].StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? parts[0][2..] : parts[0];
            if (!uint.TryParse(addressText, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var address) ||
                (address & 3) != 0)
                throw new InvalidDataException($"{path}:{lineNumber}: '{parts[0]}' is not an instruction address");
            var after = parts[1] switch
            {
                "before" => false,
                "after" => true,
                _ => throw new InvalidDataException($"{path}:{lineNumber}: '{parts[1]}' is neither before nor after")
            };
            var register = parts[2].ToLowerInvariant();
            if (register.Length < 2 || register[0] != 'r' ||
                !int.TryParse(register.AsSpan(1), NumberStyles.None, CultureInfo.InvariantCulture, out var gpr) ||
                gpr is < 0 or > 31)
                throw new InvalidDataException($"{path}:{lineNumber}: '{parts[2]}' is not a general-purpose register");
            if (taps.Any(tap => tap.Address == address && tap.After == after && tap.Register == register))
                throw new InvalidDataException($"{path}:{lineNumber}: the same tap is listed twice");
            taps.Add(new CodeTap(taps.Count, address, after, register));
        }
        return taps;
    }

    /// <summary>The instruction's IR with its taps around it (unchanged when it has none).</summary>
    public static IReadOnlyList<IrInstruction> Apply(uint address, string disassembly, IReadOnlyList<IrInstruction> ir)
    {
        if (!s_byAddress.TryGetValue(address, out var taps)) return ir;
        var uses = new HashSet<string>(ir.SelectMany(IrRegisterDataFlow.Uses).Select(IrRegisterDataFlow.BaseName));
        var definitions = new HashSet<string>(ir.SelectMany(IrRegisterDataFlow.Definitions).Select(IrRegisterDataFlow.BaseName));
        var transfersControl = ir.Any(static instruction => instruction is IrBranch or IrJump or IrReturn or
            IrIndirectJump or IrJumpTable or IrCall or IrIndirectCall);
        var result = new List<IrInstruction>(ir.Count + taps.Length);
        foreach (var tap in taps.Where(static tap => !tap.After))
        {
            if (!uses.Contains(tap.Register))
                throw new InvalidDataException(
                    $"code tap 0x{address:X8} before {tap.Register}: '{disassembly}' does not read {tap.Register}");
            result.Add(TapInstruction(tap));
        }
        result.AddRange(ir);
        foreach (var tap in taps.Where(static tap => tap.After))
        {
            if (!definitions.Contains(tap.Register))
                throw new InvalidDataException(
                    $"code tap 0x{address:X8} after {tap.Register}: '{disassembly}' does not write {tap.Register}");
            if (transfersControl)
                throw new InvalidDataException(
                    $"code tap 0x{address:X8} after {tap.Register}: '{disassembly}' transfers control; tap the next instruction instead");
            result.Add(TapInstruction(tap));
        }
        return result;
    }

    private static IrInstruction TapInstruction(CodeTap tap) =>
        new IrBinary(tap.Register, IrValue.Register(tap.Register), IrValue.Imm(tap.Id), TapOp);
}
