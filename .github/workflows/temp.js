const { owner, repo } = context.repo;
const run_id = ${{ github.event.workflow_run.id }};
const pull_head_sha = '${{github.event.workflow_run.head_sha}}';

const issue_number = await(async () => {
    const pulls = await github.rest.pulls.list({ owner, repo });
    for await (const { data } of github.paginate.iterator(pulls)) {
        for (const pull of data) {
            if (pull.head.sha === pull_head_sha) {
                return pull.number;
            }
        }
    }
})();
if (issue_number) {
    core.info(`Using pull request ${issue_number}`);
} else {
    return core.error(`No matching pull request found`);
}

const { data: { artifacts } } = await github.rest.actions.listWorkflowRunArtifacts({ owner, repo, run_id });
if (!artifacts.length) {
    return core.error(`No artifacts found`);
}
let body = `Download the artifacts for this pull request:\n`;
let hidden_windows_artifacts = `\n\n <details><summary>Windows</summary>\n`;
for (const art of artifacts) {
    if (art.name.includes('w64')) {
        hidden_windows_artifacts += `\n* [${art.name}](https://nightly.link/${owner}/${repo}/actions/artifacts/${art.id}.zip)`;
    } else {
        body += `\n* [${art.name}](https://nightly.link/${owner}/${repo}/actions/artifacts/${art.id}.zip)`;
    }
}
hidden_windows_artifacts += `\n</details>`;
body += hidden_windows_artifacts;

const { data: comments } = await github.rest.issues.listComments({ repo, owner, issue_number });
const existing_comment = comments.find((c) => c.user.login === 'github-actions[bot]');
if (existing_comment) {
    core.info(`Updating comment ${existing_comment.id}`);
    await github.rest.issues.updateComment({ repo, owner, comment_id: existing_comment.id, body });
} else {
    core.info(`Creating a comment`);
    await github.rest.issues.createComment({ repo, owner, issue_number, body });
}
