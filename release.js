const releaseLine = document.getElementById("release-line");
const releaseNotes = document.getElementById("release-notes");

function releaseDate(value) {
  return new Intl.DateTimeFormat("en-GB", {
    day: "numeric", month: "long", year: "numeric", timeZone: "UTC"
  }).format(new Date(value));
}

function noteText(text) {
  return text
    .replace(/!?\[([^\]]*)\]\([^)]+\)/g, "$1")
    .replace(/(\*\*|__|\x60)(.+?)\1/g, "$2");
}

function renderNotes(markdown) {
  const content = document.createDocumentFragment();
  let list = null;
  let paragraph = null;
  for (const rawLine of markdown.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) {
      list = paragraph = null;
      continue;
    }
    const heading = /^(#{1,6})\s+(.+)$/.exec(line);
    const item = /^([-*+]|\d+\.)\s+(.+)$/.exec(line);
    if (heading) {
      list = paragraph = null;
      const level = Math.max(3, Math.min(6, heading[1].length + 1));
      const title = document.createElement("h" + level);
      title.textContent = noteText(heading[2]);
      content.append(title);
    } else if (item) {
      paragraph = null;
      const type = /^\d/.test(item[1]) ? "ol" : "ul";
      if (!list || list.localName !== type) {
        list = document.createElement(type);
        content.append(list);
      }
      const entry = document.createElement("li");
      entry.textContent = noteText(item[2]);
      list.append(entry);
    } else {
      list = null;
      if (!paragraph) {
        paragraph = document.createElement("p");
        content.append(paragraph);
      }
      paragraph.append((paragraph.textContent ? " " : "") + noteText(line));
    }
  }
  return content;
}

// The first published release also includes test releases, unlike /latest.
fetch("https://api.github.com/repos/Shmargus/GSRecomp/releases?per_page=1")
  .then(response => {
    if (!response.ok) throw new Error("Release request failed");
    return response.json();
  })
  .then(([release]) => {
    if (!release?.tag_name) throw new Error("No release found");
    const date = releaseDate(release.published_at || release.created_at);

    if (releaseLine) {
      const assets = release.assets || [];
      const zips = assets.filter(asset => /\.zip$/i.test(asset.name));
      const linux = zips.find(asset => /linux/i.test(asset.name));
      const windows = zips.find(asset => /windows/i.test(asset.name)) ||
        zips.find(asset => !/linux/i.test(asset.name));
      if (windows && linux) {
        document.getElementById("download-windows").href = windows.browser_download_url;
        document.getElementById("download-linux").href = linux.browser_download_url;
        releaseLine.textContent =
          `${release.prerelease ? "Test release" : "Release"} ${release.tag_name}, ${date}`;
      }
    }

    if (releaseNotes) {
      const title = release.name || release.tag_name;
      document.getElementById("release-title").textContent = title.startsWith(release.tag_name)
        ? `Release ${title}` : title;
      document.getElementById("release-date").textContent = date;
      const source = document.getElementById("release-source");
      source.href = release.html_url;
      source.parentElement.hidden = false;
      if (release.body?.trim()) {
        releaseNotes.replaceChildren(renderNotes(release.body));
      } else {
        releaseNotes.textContent = "See GitHub for the release details.";
      }
    }
  })
  .catch(() => {
    if (releaseNotes) releaseNotes.textContent = "Couldn’t load release notes. Open GitHub to see them.";
  });
