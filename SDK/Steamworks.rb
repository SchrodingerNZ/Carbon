#
# Optional, privately installed Steamworks SDK discovery for SDK packaging.
# Only explicitly listed runtime libraries may enter a generated Carbon SDK.
#

module Steamworks
  def self.sdk_path
    File.join(REPOSITORY_ROOT, 'Dependencies', 'Steamworks')
  end

  def self.required_files
    [
      'public/steam/steam_api.h',
      'redistributable_bin/steam_api.dll',
      'redistributable_bin/steam_api.lib',
      'redistributable_bin/win64/steam_api64.dll',
      'redistributable_bin/win64/steam_api64.lib'
    ]
  end

  def self.available?
    required_files.all? { |file| File.file?(File.join(sdk_path, file)) }
  end

  def self.runtime_files
    return {} unless available?

    {
      'steam_api.dll' => File.join(sdk_path, 'redistributable_bin', 'steam_api.dll'),
      'steam_api64.dll' => File.join(sdk_path, 'redistributable_bin', 'win64', 'steam_api64.dll')
    }
  end

  def self.report
    if available?
      puts 'Steamworks: optional x86/x64 SDK detected; runtime DLLs will be packaged.'
    elsif Dir.exist?(sdk_path)
      puts 'Steamworks: directory found but incomplete; optional integration disabled.'
      required_files.each do |file|
        puts "  Missing: #{file}" unless File.file?(File.join(sdk_path, file))
      end
    else
      puts 'Steamworks: not installed; normal Carbon SDK build.'
    end
  end
end
