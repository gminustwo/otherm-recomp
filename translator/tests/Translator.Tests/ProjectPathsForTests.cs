namespace Translator.Tests;

internal static class ProjectPathsForTests
{
    public static string FindRepositoryRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "translator", "Translator.sln")) &&
                File.Exists(Path.Combine(dir.FullName, "project", "recomp.yml")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException("Could not find repository root.");
    }
}
