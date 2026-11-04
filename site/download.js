// Points the download buttons at the files of the latest release, found with
// the GitHub API. Without it (offline, rate limit), the buttons keep their
// link to the releases page.
(function () {
    const repo = "Zovelik/QuickEdit";
    const buttons = document.querySelectorAll("[data-asset]");
    const version = document.querySelectorAll("[data-version]");

    if (!buttons.length && !version.length) return;

    const isLinux = /Linux/.test(navigator.userAgent) && !/Android/.test(navigator.userAgent);
    document.querySelectorAll("[data-os]").forEach(function (el) {
        if ((el.dataset.os === "linux") === isLinux) el.classList.add("primary");
    });

    fetch("https://api.github.com/repos/" + repo + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
        .then(function (response) { if (!response.ok) throw new Error(response.status); return response.json(); })
        .then(function (release) {
            const assets = release.assets || [];

            buttons.forEach(function (button) {
                const suffix = button.dataset.asset;
                const asset = assets.find(function (a) { return a.name.endsWith(suffix); });
                if (!asset) return;
                button.href = asset.browser_download_url;
                const size = button.querySelector(".size");
                if (size) size.textContent = (asset.size / 1048576).toFixed(0) + " MB";
            });

            version.forEach(function (el) {
                el.textContent = release.tag_name + " · ";
                el.hidden = false;
            });
        })
        .catch(function () { /* the links to the releases page stay */ });
})();
