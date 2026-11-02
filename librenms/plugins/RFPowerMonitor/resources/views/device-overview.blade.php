{{-- "Switch" box on the device Overview: the LLDP neighbour, rows styled like the system box --}}
<x-device.overview.panel :title="$title" icon="fa fa-sitemap" :href="route('device', ['device' => $device, 'tab' => 'neighbours'])">
    <div class="tw:divide-y tw:divide-gray-300 tw:dark:divide-zinc-800">
        @forelse($links as $link)
            <div class="tw:grid tw:grid-cols-3 tw:items-center tw:gap-2.5 tw:px-2 tw:py-1 tw:hover:bg-neutral-100 tw:dark:hover:bg-dark-gray-300">
                <div class="tw:font-medium">{{ __('Name') }}</div>
                <div class="tw:col-span-2 tw:min-w-0 tw:break-words">
                    @if($link->remoteDevice)
                        <x-device-link :device="$link->remoteDevice" />
                    @else
                        {{ $link->remote_hostname }}
                    @endif
                </div>
            </div>
            <div class="tw:grid tw:grid-cols-3 tw:items-center tw:gap-2.5 tw:px-2 tw:py-1 tw:hover:bg-neutral-100 tw:dark:hover:bg-dark-gray-300">
                <div class="tw:font-medium">{{ __('Port') }}</div>
                <div class="tw:col-span-2 tw:min-w-0 tw:break-words">
                    @if($link->remotePort)
                        <x-port-link :port="$link->remotePort" />
                    @else
                        {{ $link->remote_port }}
                    @endif
                </div>
            </div>
            @if($link->remote_version)
                <div class="tw:grid tw:grid-cols-3 tw:items-center tw:gap-2.5 tw:px-2 tw:py-1 tw:hover:bg-neutral-100 tw:dark:hover:bg-dark-gray-300">
                    <div class="tw:font-medium">{{ __('System') }}</div>
                    <div class="tw:col-span-2 tw:min-w-0 tw:break-words">{{ $link->remote_version }}</div>
                </div>
            @endif
            <div class="tw:grid tw:grid-cols-3 tw:items-center tw:gap-2.5 tw:px-2 tw:py-1 tw:hover:bg-neutral-100 tw:dark:hover:bg-dark-gray-300">
                <div class="tw:font-medium">{{ __('Seen with') }}</div>
                <div class="tw:col-span-2 tw:min-w-0 tw:break-words">
                    {{ strtoupper($link->protocol) }}@if($link->port) {{ __('on') }} <x-port-link :port="$link->port" />@endif
                </div>
            </div>
        @empty
            <div class="tw:px-2 tw:py-1">{{ __('No switch seen (LLDP) at the last discovery') }}</div>
        @endforelse
    </div>
</x-device.overview.panel>
