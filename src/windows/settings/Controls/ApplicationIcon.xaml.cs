using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using StoneSettings.Services;
using Windows.Storage.Streams;

namespace StoneSettings.Controls;

public sealed partial class ApplicationIcon : UserControl
{
    public static readonly DependencyProperty IconPngProperty = DependencyProperty.Register(
        nameof(IconPng), typeof(string), typeof(ApplicationIcon), new PropertyMetadata("", IconChanged));
    public string IconPng
    {
        get => (string)GetValue(IconPngProperty);
        set => SetValue(IconPngProperty, value);
    }
    private int revision;
    internal bool HasImage => Picture.Visibility == Visibility.Visible;

    public ApplicationIcon() => InitializeComponent();

    private static void IconChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
        => ((ApplicationIcon)sender).LoadIcon();

    private async void LoadIcon()
    {
        int current = ++revision;
        Picture.Source = null;
        Picture.Visibility = Visibility.Collapsed;
        Fallback.Visibility = Visibility.Visible;
        byte[] bytes = ApplicationIconData.Decode(IconPng);
        if (bytes.Length == 0)
            return;
        try
        {
            using InMemoryRandomAccessStream stream = new();
            using (DataWriter writer = new(stream.GetOutputStreamAt(0)))
            {
                writer.WriteBytes(bytes);
                await writer.StoreAsync();
            }
            stream.Seek(0);
            BitmapImage bitmap = new();
            await bitmap.SetSourceAsync(stream);
            // A recycled list item may have received another application meanwhile.
            if (current != revision)
                return;
            Picture.Source = bitmap;
            Picture.Visibility = Visibility.Visible;
            Fallback.Visibility = Visibility.Collapsed;
        }
        catch (Exception error) when (error is IOException or ArgumentException or System.Runtime.InteropServices.COMException)
        {
            // An unavailable or invalid icon must not prevent application management.
        }
    }
}
