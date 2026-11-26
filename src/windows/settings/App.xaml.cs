using Microsoft.UI.Xaml;

namespace StoneSettings;

public partial class App : Application
{
    private Window? window;
    public App()
    {
        try
        {
            string[] arguments = Environment.GetCommandLineArgs();
            if (arguments.Any(value => value.StartsWith("--runtime-features-", StringComparison.Ordinal) || value == "--runtime-java-apk"))
                throw new ArgumentException("Runtime feature controls have been removed from StoneSettings.");
            Helpers.UiText.Configure(arguments);
        }
        catch (ArgumentException error)
        {
            Console.Error.WriteLine(error.Message);
            Environment.Exit(2);
        }
        InitializeComponent();
    }
    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        string[] arguments = Environment.GetCommandLineArgs();
        int activation = Array.IndexOf(arguments, "--notification-activate");
        if (activation >= 0)
        {
            int result = 1;
            try
            {
                if (activation + 1 >= arguments.Length)
                    throw new InvalidDataException("Missing notification activation.");
                int diagnostic = Array.IndexOf(arguments, "--notification-activation-test-server");
                await Services.NotificationActivation.ActivateAsync(arguments[activation + 1],
                    diagnostic >= 0 && diagnostic + 1 < arguments.Length ? arguments[diagnostic + 1] : null);
                result = 0;
            }
            catch (Exception error)
            {
                System.Diagnostics.Trace.WriteLine(error);
            }
            finally
            {
                Environment.Exit(result);
            }
            return;
        }
        if (arguments.Contains("--confirmation-pipe"))
        {
            window = new ConfirmationWindow(arguments);
            window.Activate();
            return;
        }
        string dataRoot = Services.SystemImageSelection.DataRoot(arguments);
        string? image = await Services.SystemImageSelection.ResolveAsync(arguments, dataRoot);
        if (image is null)
        {
            WelcomeWindow welcome = new(dataRoot);
            welcome.ImageSelected += selected =>
            {
                window = new MainWindow(selected);
                window.Activate();
                welcome.Close();
            };
            window = welcome;
        }
        else
            window = new MainWindow(image);
        window.Activate();
    }
}
