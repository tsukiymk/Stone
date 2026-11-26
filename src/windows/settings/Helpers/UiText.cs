using System.Globalization;
using Microsoft.Windows.ApplicationModel.Resources;
using Microsoft.Windows.Globalization;
using StoneSettings.Helpers;
using StoneSettings.Models;
using StoneSettings.Services;
using StoneSettings.Utils;

namespace StoneSettings.Helpers;

// MRT also supplies x:Uid resources. Set the process override before creating
// any XAML or ResourceLoader; it is deliberately not saved as Android state.
internal static class UiText
{
    private static readonly Lazy<ResourceLoader> resources = new(() => new ResourceLoader());

    internal static void Configure(string[] arguments)
    {
        int index = Array.IndexOf(arguments, "--language");
        string language = "";
        if (index >= 0)
        {
            if (index + 1 >= arguments.Length || arguments.Count(value => value == "--language") != 1)
                throw new ArgumentException("Expected --language system|en-US|zh-CN|ja-JP.");
            language = arguments[index + 1] switch
            {
                "system" => "",
                "en-US" => "en-US",
                "zh-CN" => "zh-CN",
                "ja-JP" => "ja-JP",
                _ => throw new ArgumentException("Expected --language system|en-US|zh-CN|ja-JP.")
            };
        }
        // Unpackaged apps start without an override. This SDK rejects assigning
        // an empty tag, so system mode leaves its display-language lookup intact.
        if (language.Length != 0)
            ApplicationLanguages.PrimaryLanguageOverride = language;
    }

    internal static string Get(string key)
    {
        string value = resources.Value.GetString(key);
        return string.IsNullOrEmpty(value) ? throw new InvalidOperationException("Missing UI resource: " + key) : value;
    }

    internal static string Format(string key, params object[] arguments) =>
        string.Format(CultureInfo.CurrentCulture, Get(key), arguments);
}
