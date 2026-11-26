using System.Text.Json.Serialization;

namespace StoneSettings.Models;

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(AppCatalog))]
[JsonSerializable(typeof(ConfirmationEnvelope))]
[JsonSerializable(typeof(PackageOperationResult))]
internal partial class SettingsJson : JsonSerializerContext
{
}
