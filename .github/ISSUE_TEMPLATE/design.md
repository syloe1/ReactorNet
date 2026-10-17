---
name: 设计任务
about: 一份需要先定方案、再落代码的设计（RFC / ADR / 边界决策）
title: 'docs: '
labels: ''
assignees: ''
---

## Goal

这条设计要定死什么。一句话，写成一个**可被后续 PR 引用**的结论。

## Background

为什么必须在写代码之前定下来。写清「如果现在不定，会在哪一步返工」。

## Scope

这份文档覆盖的范围。列出要产出的章节 / 对比表 / 接口草案。

## Non-Goals

明确排除的东西。对设计任务尤其重要：**没有边界的文档会变成百科**。
写出「为什么不在这条里做」，而不只是「不做什么」。

## Design

方案的正文或要点。涉及多个方案时，用表格横向对比，并写清**选定哪一个、为什么**。

## API

文档中要钉死的接口签名。没有 API 的纯文档写「无」。

## Dependencies

Depends on: ｜ Blocks:

依赖必须写 issue 编号。被阻塞的 issue 在本文档合并前不应开工。

## Implementation Tasks

- [ ] 具体章节 / 对比表 / 示意图

## Tests

文档类任务通常写「无」。若文档中的结论需要实验佐证，在这里写出实验方法。

## Acceptance Criteria

- [ ] 每条结论都能被另一个人在执行时直接照做
- [ ] 关键取舍有理由，且理由不是「太难」「以后再说」

## Expected PR

一个 markdown 文件 / 一篇 ADR / 零代码。

## Interview Knowledge

- 这份文档对应的高频面试问题
- 为什么这个决定值得单独成文
